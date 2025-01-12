/*************************************************************************************
    Author: Coder LSF (Liu Shaofeng)
      Date: 2023/10/16
**************************************************************************************/

#include "transformer.h"

#include <string.h>
#include <math.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "log.h"
#include "utility.h"
#include "ftdebug.h"

namespace cpuft {

bool ParallelTransformer::load(std::string_view ckpt_path, std::string_view tknr_path, ModelFileType mft,
        QuantType qt, int num_threads, bool use_numa, int max_batch_size, uint64_t rand_seed) {

    _max_batch_size = max_batch_size;

    TransformerModel tf(_debug);
    if (!tf.load(ckpt_path, tknr_path, mft)) {
        return false;
    }
    tf.conf.max_seq_len = 1024;

    _tkn = std::move(tf.tokenizer);

    if (tf.conf.quant_type == QuantType::NONE) {
        tf.conf.quant_type = qt;
    }

    _sampler.build(tf.conf.vocab_size, rand_seed);
    return _tp.init(this, tf, num_threads, use_numa);
}

std::vector<int> ParallelTransformer::encode(const char* prompt) const noexcept {
    if (prompt == nullptr || prompt[0] == '\0') {
        return {};
    }
    std::vector<int> res_tokens(size_t(_tfc.max_seq_len), 0);
    int num_tokens = _tkn.encode(prompt, {res_tokens.data(), res_tokens.size()}, true);
    res_tokens.resize(size_t(num_tokens));
    return res_tokens;
}

bool ParallelTransformer::generate(const char* prompt,
        std::function<bool(const char* text, int num_input_tokens, int num_output_tokens, bool ended)> cb,
        int max_new_tokens, float temperature, float topp) {

    auto input_tokens = encode(prompt);
    if (input_tokens.empty()) {
        fprintf(stderr, "Empty input for generate()\n");
        return false;
    }

    printf("Input prompt:%s\n", prompt);
    print_vector("Input tokens:", input_tokens);

    int prev_token = -1;
    auto cb_ = [&](std::span<const int> tokens, int index, bool ended) -> bool {
        auto piece = _tkn.decode(tokens[0], prev_token);
        prev_token = tokens[0];
        return cb(piece, input_tokens.size(), index + tokens.size() - input_tokens.size(), ended);
    };
    return generate(input_tokens, cb_, max_new_tokens, temperature, topp);
}

bool ParallelTransformer::generate(const std::vector<int>& input_tokens,
        std::function<bool(std::span<const int> tokens, int index, bool ended)> cb,
        int max_new_tokens, float temperature, float topp) {
    if (input_tokens.empty()) {
        ft_log_error("Empty input for generate()");
        return false;
    } else if (int(input_tokens.size()) >= _tfc.max_seq_len) {
        ft_log_error("Input is too long for generate()");
        return false;
    } else if (auto v = _tfc.max_seq_len - int(input_tokens.size()); max_new_tokens > v) {
        max_new_tokens = v;
    }

    int max_tokens = int(input_tokens.size()) + max_new_tokens;

    Tensor logits;
    std::span<const int> cur_tokens = {input_tokens.data(), input_tokens.size()};
    for (int i = 0, next_token=-1; next_token != 0 && i < max_tokens;) {
        forward(cur_tokens, i, logits);
        next_token = _sampler.sample(logits, temperature, topp);
        if (!cb({&next_token, 1}, i, next_token == 0)) {
            break;
        }
        i += cur_tokens.size();
        cur_tokens = {&next_token, 1};
    }
    return true;
}

void ParallelTransformer::forward(std::span<const int> tokens, int pos, Tensor& logits) {
    const int bs = tokens.size();

    auto qtype = _tfc.quant_type;
    auto gsize = _tfc.quant_group_size;
    SequentialAllocator mem(_tfr.buf.data());
    auto q0 = Tensor::manage(mem, _tfc.dim,  0, qtype, gsize);

    SequentialAllocator buf(mem.get());
    auto x1 = Tensor::manage(buf, _tfc.dim, bs);
    for (int i = 0; i < bs; ++i) {
        auto emb = _tfw.token_embedding_table[tokens[i]];
        if (emb.is_quantized()) {
            x1[i].dequantize(emb);
        } else {
            x1[i].copy_from(emb);
        }
    }
    {
        auto x2 = Tensor::manage(buf, _tfc.dim, bs);
        auto qx = Tensor::manage(buf, _tfc.dim, bs, qtype, gsize);
        buf.reset(buf.get());
        auto qkv = Tensor::manage(buf, _tfc.dim + _tfc.kv_dim * 2, bs);
        buf.restart();
        auto hd  = Tensor::manage(buf, _tfc.hidden_dim, bs);
        auto qh  = Tensor::manage(buf, _tfc.hidden_dim, bs, qtype, gsize);
        for (int l = 0; l < _tfc.n_layers; ++l) {
            x2.rmsnorm(x1, _tfw.rms_att_weight[l]);

            qx.quantize(x2);
            execute(TaskType::QKV,  pos, l, &qx,  &qkv);
            execute(TaskType::ATTN, pos, l, &qkv, &x2); // RoPE & ATTN

            qx.quantize(x2);
            execute(TaskType::ATTN_O, pos, l, &qx, &x1); // Attn & Add to x1
            if (bs > 1 && l == _tfc.n_layers - 1) {
                x1 = x1.slice(x1.rows() - 1, x1.rows());
            }

            x2.rmsnorm(x1, _tfw.rms_ffn_weight[l]);

            qx.quantize(x2);
            execute(TaskType::FFN13, pos, l, &qx, &hd); // FFN1 & FFN2 & SwigLU

            qh.quantize(hd);
            execute(TaskType::FFN2, pos, l, &qh, &x1); // FFN2 & Add to x1
        }
    }

    auto x = x1[-1];
    x.rmsnorm(x, _tfw.rms_final_weight);
    q0.quantize(x);

    logits.reset(_tfc.vocab_size);
    logits.manage(mem.get());
    execute(TaskType::CLS, pos, 0, &q0, &logits);
}

int compare_float(const void* a, const void* b) {
    if (*reinterpret_cast<const float*>(a) < *reinterpret_cast<const float*>(b)) {
        return -1;
    } else if (*reinterpret_cast<const float*>(a) == *reinterpret_cast<const float*>(b)) {
        return 0;
    } else {
        return 1;
    }
}

void ParallelTransformer::analyze_weights(const TransformerModel& tf) {
    auto& tfw = tf.weights;
    auto  attq = tfw.attn_q[0];
    Tensor row(attq.columns());
    Tensor row2(attq.columns());

    float midf = 0.;
    float maxf = 0.;
    for (int i = 0; i < attq.rows(); ++i) {
        row.copy_from(attq[i]);
        auto [min1, max1] = row.min_max();
        auto vmax = cpuft::simd::array_max(row.float_data(), row.size());
        if (vmax != max1) {
            printf("Found wrong max value:%.6f vs %.6f\n", vmax, max1);
        }

        midf += (max1 - min1) / 2;
        maxf += std::max(fabs(max1), fabs(min1));
        row.add(-min1);
        row.multiply(1./(max1 - min1));

        if ((i & 7) == 0) {
            if (i != 0) {
                auto [vmin, vmax] = row2.min_max();
                printf("merged min:%6.3f\tmax:%6.3f\n", vmin, vmax);
            }
            row2.copy_from(row);
        } else {
            row2.add(row);
        }
    }
    midf /= attq.rows();
    maxf /= attq.rows();
    printf("midf:%.5f\tmaxf:%.5f\n", midf, maxf);
}

bool ParallelTransformer::parallel_global_init(
            const TransformerModel& tf, int num_threads, bool use_numa, ThreadGroupBuilder& tgb) {
    _num_threads = num_threads;
    _use_numa    = use_numa && is_numa_available();

    _tfc = tf.conf;
    if (_tfc.n_heads % _tfc.n_kv_heads != 0) {
        fprintf(stderr, "Unsupported value of n_heads:%d and n_kv_heads:%d\n", _tfc.n_heads, _tfc.n_kv_heads);
        return false;
    }

    auto& tfw = tf.weights;
    _tfw.token_embedding_table = std::move(tfw.token_embedding_table);
    _tfw.rms_att_weight        = std::move(tfw.attn_norm);
    _tfw.rms_ffn_weight        = std::move(tfw.ffn_norm);
    _tfw.rms_final_weight      = std::move(tfw.out_norm);
    _tfw.rope_freq_cis         = std::move(tfw.rope_freq_cis);

    const int BASE_COMPUTATION_SIZE = 36864 * 2 / 3;

    int tn = int(size_t(_tfc.dim) * _tfc.dim / BASE_COMPUTATION_SIZE);
    if (tn < num_threads) {
        set_tt_gid(TaskType::ATTN_O, tgb.add_group(tn));
    }

    tn = _tfc.n_kv_heads < num_threads ? _tfc.n_kv_heads : num_threads;
    if (tn < num_threads) {
        set_tt_gid(TaskType::ATTN, tgb.add_group(tn));
    }

    tn = int(size_t(_tfc.dim) * _tfc.hidden_dim / BASE_COMPUTATION_SIZE);
    if (tn < num_threads) {
        set_tt_gid(TaskType::FFN2, tgb.add_group(tn));
    }

    bool ret = _tfr.buf.alloc(sizeof(float) * (_tfc.dim * 3 + _tfc.vocab_size) * _max_batch_size, false);
    if (!ret) {
        ft_log_error("Out of memory");
        return false;
    }

    return true;
}

bool ParallelTransformer::parallel_thread_init(
        const std::vector<ThreadGroupInfo>& tgis, const TransformerModel& tf, ThreadData& td) {
    if (_use_numa) {
        td.w.qkv.set_memory_type(MemoryType::NUMA);
        td.w.attn_o.set_memory_type(MemoryType::NUMA);
        td.w.ffn_1.set_memory_type(MemoryType::NUMA);
        td.w.ffn_2.set_memory_type(MemoryType::NUMA);
        td.w.ffn_3.set_memory_type(MemoryType::NUMA);
        td.w.cls.set_memory_type(MemoryType::NUMA);
    }

    auto split_rows = [this, &tgis](TaskType tt, int num_rows) -> std::tuple<int, int> {
        const ThreadGroupInfo* tgi = nullptr;
        for (auto& item : tgis) {
            if (item.group_id == tt2gid(tt)) {
                tgi = &item;
                break;
            }
        }
        if (tgi == nullptr) {
            return {0, 0};
        }

        int itv = num_rows / tgi->num_threads;
        int rem = num_rows % tgi->num_threads;
        std::pair<int, int> res;
        if (tgi->thread_id < rem) {
            res.first  = (itv + 1) * tgi->thread_id; // start (offset)
            res.second = itv + 1; // num
        } else {
            res.first  = (itv + 1) * rem + itv * (tgi->thread_id - rem);
            res.second = itv;
        }
        return res;
    };

    auto copy_layers = [](Tensor& t, const Tensor &s, int copied, int offset, int rows) -> int {
        if (offset < 0 || offset >= s.rows() || rows < 1) {
            return 0;
        }
        auto nr = std::min(s.rows() - offset, rows);
        for (int i = 0; i < s.layers(); ++i) {
            auto src = s[i].slice(offset, offset+nr);
            auto tgt = t[i].slice(copied, copied+nr); // copied: t offset
            if (tgt.is_quantized() && !src.is_quantized()) {
                tgt.quantize(src);
            } else {
                src.copy_to(tgt);
            }
        }
        return nr;
    };

    auto& tfw = tf.weights;
    if (auto [offset, rows] = split_rows(TaskType::QKV, _tfc.dim + _tfc.kv_dim * 2); rows > 0) {
        td.c.qkv_offset = offset;
        auto qtype = tfw.attn_q.is_quantized() ? tfw.attn_q.quant_type() : _tfc.quant_type;
        td.w.qkv.reset(tfw.attn_q.columns(), rows, _tfc.n_layers, qtype, _tfc.quant_group_size);
        if (!td.w.qkv.reserve_memory()) {
            ft_log_error("Out of memory for QKV weights of thread:%d", tgis[0].thread_id);
            return false;
        }

        int nr = copy_layers(td.w.qkv, tfw.attn_q, 0, offset, rows); // copy q
        nr += copy_layers(td.w.qkv, tfw.attn_k, nr, offset + nr - _tfc.dim, rows - nr); // copy k
        copy_layers(td.w.qkv, tfw.attn_v, nr, offset + nr - _tfc.dim - _tfc.kv_dim, rows - nr);
    }
    if (auto [offset, rows] = split_rows(TaskType::ATTN_O, _tfc.dim); rows > 0) {
        td.c.attn_o_offset = offset;
        auto qtype = tfw.attn_o.is_quantized() ? tfw.attn_o.quant_type() : _tfc.quant_type;
        td.w.attn_o.reset(tfw.attn_o.columns(), rows, _tfc.n_layers, qtype, _tfc.quant_group_size);
        if (!td.w.attn_o.reserve_memory()) {
            ft_log_error("Out of memory for WO weights of thread:%d", tgis[0].thread_id);
            return false;
        }
        copy_layers(td.w.attn_o, tfw.attn_o, 0, offset, rows);
    }
    if (auto [offset, rows] = split_rows(TaskType::FFN13, _tfc.hidden_dim); rows > 0) {
        td.c.ffn_13_offset = offset;
        auto qtype = tfw.ffn_1.is_quantized() ? tfw.ffn_1.quant_type() : _tfc.quant_type;
        td.w.ffn_1.reset(tfw.ffn_1.columns(), rows, _tfc.n_layers, qtype, _tfc.quant_group_size);
        td.w.ffn_3.reset(tfw.ffn_3.columns(), rows, _tfc.n_layers, qtype, _tfc.quant_group_size);
        if (!td.w.ffn_1.reserve_memory() || !td.w.ffn_3.reserve_memory()) {
            ft_log_error("Out of memory for W1 and W3 weights of thread:%d", tgis[0].thread_id);
            return false;
        }
        copy_layers(td.w.ffn_1, tfw.ffn_1, 0, offset, rows);
        copy_layers(td.w.ffn_3, tfw.ffn_3, 0, offset, rows);
    }
    if (auto [offset, rows] = split_rows(TaskType::FFN2, _tfc.dim); rows > 0) {
        td.c.ffn_2_offset = offset;
        auto qtype = tfw.ffn_2.is_quantized() ? tfw.ffn_2.quant_type() : _tfc.quant_type;
        td.w.ffn_2.reset(tfw.ffn_2.columns(), rows, _tfc.n_layers, qtype, _tfc.quant_group_size);
        if (!td.w.ffn_2.reserve_memory()) {
            ft_log_error("Out of memory for FFN2 weights of thread:%d", tgis[0].thread_id);
            return false;
        }
        copy_layers(td.w.ffn_2, tfw.ffn_2, 0, offset, rows);
    }
    if (auto [offset, rows] = split_rows(TaskType::CLS, _tfc.vocab_size); rows > 0) {
        td.c.cls_offset = offset;
        auto qtype = tfw.classifier.is_quantized() ? tfw.classifier.quant_type() : _tfc.quant_type;
        td.w.cls.reset(tfw.classifier.columns(), rows, qtype, _tfc.quant_group_size);
        if (!td.w.cls.reserve_memory()) {
            ft_log_error("Out of memory for WCLS weights of thread:%d", tgis[0].thread_id);
            return false;
        }
        if (tfw.classifier.is_quantized()) {
            td.w.cls.copy_from(tfw.classifier.slice(offset, offset+rows));
        } else {
            td.w.cls.quantize(tfw.classifier.slice(offset, offset+rows));
        }
    }
    if (auto [offset, heads] = split_rows(TaskType::ATTN, _tfc.n_kv_heads); heads > 0) {
        td.c.kv_heads_offset = offset;
        td.r.k_cache.reset(_tfc.head_size, heads * _tfc.max_seq_len, _tfc.n_layers);
        td.r.v_cache.reset(_tfc.head_size, heads * _tfc.max_seq_len, _tfc.n_layers);
        if (!td.r.k_cache.reserve_memory() || !td.r.v_cache.reserve_memory()) {
            ft_log_error("Out of memory for KV Cache of thread:%d", tgis[0].thread_id);
            return false;
        }
    }

    auto buf_size1 = std::max(_tfc.dim, _tfc.hidden_dim) * 3;
    auto buf_size2 = _tfc.dim * 2 + std::max(std::max(td.w.qkv.rows(), td.w.ffn_1.rows()*2), td.w.cls.rows());
    if (!td.r.buf.alloc(sizeof(float) * std::max(buf_size1, buf_size2) * _max_batch_size, _use_numa)) {
        ft_log_error("Out of memory for thread runtime buffer");
        return false;
    }

    return true;
}

void ParallelTransformer::execute_qkv(ThreadData& td, Task t, const ThreadGroupInfo& tgi) const noexcept {
    auto w      = td.w.qkv[t.layer];
    int  offset = td.c.qkv_offset;

    auto& x = *t.t1;
    auto& o = *t.t2;
    auto  tmp = Tensor::manage(td.r.buf.data(), w.rows(), x.rows());
    w.matmul(x, tmp);
    o.copy_from(tmp, offset);
}

void ParallelTransformer::execute_attn(ThreadData& td, Task t, const ThreadGroupInfo& tgi) const noexcept {
    auto buf = SequentialAllocator(td.r.buf.data());

    auto kc = td.r.k_cache[t.layer];
    auto vc = td.r.v_cache[t.layer];

    auto& iqkv = *t.t1;
    auto& io   = *t.t2;

    const int hgs = _tfc.n_heads / _tfc.n_kv_heads;
    const int pos = t.pos;
    const int bs  = iqkv.rows() < 1 ? 1 : iqkv.rows();
    const int seqlen = pos + bs;

    const int kv_heads = kc.rows() / _tfc.max_seq_len;

    auto q   = Tensor::manage(buf, _tfc.head_size, bs, hgs);
    auto att = Tensor::manage(buf, seqlen, bs, hgs);

    float attn_scale = 1. / sqrtf(_tfc.head_size);
    auto o   = Tensor::manage(buf, _tfc.head_size, bs, hgs);
    for (int ihead = 0; ihead < kv_heads; ++ihead) {
        const int coff = _tfc.max_seq_len*ihead;
        auto kl = kc.slice(coff, coff + seqlen);
        auto vl = vc.slice(coff, coff + seqlen);

        auto k = kl.slice(pos, seqlen);
        auto v = vl.slice(pos, seqlen);

        for (int i = 0; i < hgs; ++i) {
            auto t = q[i];
            iqkv.copy_to(t, _tfc.head_size * ((td.c.kv_heads_offset + ihead) * hgs + i));
        }

        iqkv.copy_to(k, _tfc.head_size * (td.c.kv_heads_offset + ihead) + _tfc.dim);
        iqkv.copy_to(v, _tfc.head_size * (td.c.kv_heads_offset + ihead) + _tfc.dim + _tfc.kv_dim);

        if constexpr (false) {
            q.sequence_rope(_tfw.rope_freq_cis, pos);
            k.sequence_rope(_tfw.rope_freq_cis, pos);
        } else {
            q.sequence_rope_v2(pos, _tfc.head_size, _tfc.max_seq_len);
            k.sequence_rope_v2(pos, _tfc.head_size, _tfc.max_seq_len);
        }

        kl.matmul(q, att);
        att.multiply(attn_scale);
        for (int i = 0; i < att.rows(); ++i) {
            for (int j = 0; j < hgs; ++j) {
                att[j][i].softmax(pos+i+1);
            }
        }
        vl.weighted_sum(att, o, 0, 1e-15);
        for (int i = 0; i < hgs; ++i) {
            auto t = o[i];
            io.copy_from(t, _tfc.head_size * ((td.c.kv_heads_offset + ihead) * hgs + i));
        }
    }
}

void ParallelTransformer::execute_attn_o(ThreadData& td, Task t, const ThreadGroupInfo&) const noexcept {
    auto   w      = td.w.attn_o[t.layer];
    int    offset = td.c.attn_o_offset;

    auto& x = *t.t1;
    auto& o = *t.t2;
    auto  tmp = Tensor::manage(td.r.buf.data(), w.rows(), x.rows());
    w.matmul(x, tmp);
    o.add(tmp, offset);
}

void ParallelTransformer::execute_ffn13(ThreadData& td, Task t, const ThreadGroupInfo&) const noexcept {
    auto   w1     = td.w.ffn_1[t.layer];
    auto   w3     = td.w.ffn_3[t.layer];
    int    offset = td.c.ffn_13_offset;

    auto& x = *t.t1;
    auto& o = *t.t2;

    SequentialAllocator buf(td.r.buf.data());
    auto  o1 = Tensor::manage(buf, w1.rows(), x.rows());
    auto  o3 = Tensor::manage(buf, w3.rows(), x.rows());
    w1.matmul(x, o1);
    w3.matmul(x, o3);
    o1.swiglu(o3);
    o.copy_from(o1, offset);
}

void ParallelTransformer::execute_ffn2(ThreadData& td, Task t, const ThreadGroupInfo&) const noexcept {
    auto   w      = td.w.ffn_2[t.layer];
    int    offset = td.c.ffn_2_offset;

    auto& x = *t.t1;
    auto& o = *t.t2;
    auto  tmp = Tensor::manage(td.r.buf.data(), w.rows(), x.rows());
    w.matmul(x, tmp);
    o.add(tmp, offset);
}

void ParallelTransformer::execute_cls(ThreadData& td, Task t, const ThreadGroupInfo&) const noexcept {
    auto&  w      = td.w.cls;
    int    offset = td.c.cls_offset;

    auto& x = *t.t1;
    auto& o = *t.t2;
    auto  tmp = Tensor::manage(td.r.buf.data(), w.rows(), x.rows());
    w.matmul(x, tmp);
    o.copy_from(tmp, offset);
}

} // namespace cpuft
