#!/usr/bin/env python3
"""TTS 服务器 — edge-tts → 原始 PCM → HTTP 返回"""
from flask import Flask, request, Response
import edge_tts, asyncio, tempfile, subprocess, os

app = Flask(__name__)

@app.route('/v1/audio/speech', methods=['POST'])
def speech():
    data = request.json
    text = data.get('input', '')
    voice = data.get('voice', 'alloy')
    if voice in ('alloy', ''): voice = 'zh-CN-XiaoyiNeural'

    async def gen_tts():
        tmp_mp3 = tempfile.mktemp(suffix='.mp3')
        tmp_raw = tempfile.mktemp(suffix='.raw')
        try:
            comm = edge_tts.Communicate(text, voice)
            await comm.save(tmp_mp3)
            subprocess.run(['ffmpeg', '-y', '-i', tmp_mp3, '-f', 's16le',
                           '-acodec', 'pcm_s16le', '-ar', '8000', '-ac', '1', tmp_raw],
                          capture_output=True, timeout=10)
            with open(tmp_raw, 'rb') as f:
                return f.read()
        finally:
            for f in [tmp_mp3, tmp_raw]:
                if os.path.exists(f): os.unlink(f)

    pcm = asyncio.run(gen_tts())
    return Response(pcm, mimetype='audio/raw')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=9881)
