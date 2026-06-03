进入.\Jarvis2.0\vosk-server\websocket目录打开vosk语音模型
VOSK_SAMPLE_RATE=16000 python3 asr_server.py ./vosk-model-cn-0.22

开启tts服务
python3 tts_server.py

打开ollama与本地模型交互
