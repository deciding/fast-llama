#python ./tools/convert_flm.py -m ~/.cache/huggingface/hub/models--meta-llama--Llama-2-7b-hf/snapshots/01c7f73d771dfac7d292323805ebc428287df4f9/ -o ./models/Llama-2-7b-hf-int8.flm -t int8

./main -c ./models/Llama-2-7b-hf-int8.flm -j 40 -n 200 -i 'That was a long long story happened in the ancient China.'