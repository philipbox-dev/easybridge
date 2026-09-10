/*
 * Звонок в браузере: захват микрофона, отправка кадров, приём и
 * воспроизведение.
 *
 * Кодек выбирается по возможностям браузера:
 *   • Opus через WebCodecs — ~24 кбит/с, каждый кадр самодостаточен,
 *     ровно то, что нужно для потока по сокету;
 *   • иначе PCM16 16 кГц — 256 кбит/с, зато работает везде и
 *     телефону-шлюзу его проще всего пережать в AMR-NB.
 *
 * MediaRecorder намеренно не используется: он отдаёт webm-контейнер,
 * где отдельный кусок без предыдущих не декодируется, — для живого
 * потока это тупик.
 */
(function (global) {
  'use strict';

  const AUDIO_MAGIC = 0xA1;
  const HDR = 8;
  const CODEC_PCM16 = 0, CODEC_OPUS = 1;
  const SAMPLE_RATE = 16000;      // и захват, и воспроизведение PCM
  const FRAME_MS = 20;            // 320 отсчётов — стандартный размер кадра
  const JITTER_MS = 60;           // буфер против рваной сети

  // Захват в отдельном потоке аудио: в основном он бы спотыкался о
  // перерисовку чата.
  const WORKLET_SRC = `
    class Cap extends AudioWorkletProcessor {
      constructor() { super(); this.buf = []; this.need = ${SAMPLE_RATE / 1000 * FRAME_MS}; }
      process(inputs) {
        const ch = inputs[0] && inputs[0][0];
        if (!ch) return true;
        this.buf.push(...ch);
        while (this.buf.length >= this.need) {
          const chunk = this.buf.splice(0, this.need);
          this.port.postMessage(Float32Array.from(chunk));
        }
        return true;
      }
    }
    registerProcessor('cap', Cap);
  `;

  function packAudio(payload, seq, codec, fromAir) {
    const out = new Uint8Array(HDR + payload.length);
    out[0] = AUDIO_MAGIC;
    out[1] = fromAir ? 1 : 0;
    out[2] = 0; out[3] = 0;               // отправителя проставит сервер
    out[4] = (seq >> 8) & 0xFF; out[5] = seq & 0xFF;
    out[6] = codec; out[7] = 0;
    out.set(payload, HDR);
    return out;
  }

  function parseAudio(buf) {
    const u = new Uint8Array(buf);
    if (u.length < HDR || u[0] !== AUDIO_MAGIC) return null;
    return {
      fromAir: !!(u[1] & 1),
      member: (u[2] << 8) | u[3],
      seq: (u[4] << 8) | u[5],
      codec: u[6],
      payload: u.subarray(HDR),
    };
  }

  const f32ToPcm16 = f => {
    const out = new Int16Array(f.length);
    for (let i = 0; i < f.length; i++) {
      const s = Math.max(-1, Math.min(1, f[i]));
      out[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
    }
    return out;
  };

  const pcm16ToF32 = p => {
    const out = new Float32Array(p.length);
    for (let i = 0; i < p.length; i++) out[i] = p[i] / 0x8000;
    return out;
  };

  class CallClient {
    constructor(ws, opts) {
      this.ws = ws;
      this.on = opts || {};
      this.active = false;
      this.seq = 0;
      this.muted = false;
      this.codec = CODEC_PCM16;
      this.ctx = null;
      this.stream = null;
      this.node = null;
      this.encoder = null;
      this.decoders = new Map();     // отправитель → AudioDecoder
      this.playAt = 0;
      this.lastLevel = 0;
    }

    static get supported() {
      return !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia
                && global.AudioContext);
    }

    // ── микрофон ──
    /** Сервер может потребовать конкретный кодек: когда в звонке есть
     *  эфирное плечо, мост на телефоне понимает только PCM16. */
    forceCodec(codec) {
      if (codec === this.codec) return;
      this.codec = codec;
      if (codec === CODEC_PCM16 && this.encoder) {
        try { this.encoder.close(); } catch (e) {}
        this.encoder = null;
      }
    }

    async start() {
      if (this.active) return;
      this.stream = await navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: true, noiseSuppression: true,
                 autoGainControl: true, channelCount: 1 },
      });
      this.ctx = new AudioContext({ sampleRate: SAMPLE_RATE });
      await this.ctx.audioWorklet.addModule(
        URL.createObjectURL(new Blob([WORKLET_SRC], { type: 'text/javascript' })));

      const src = this.ctx.createMediaStreamSource(this.stream);
      this.node = new AudioWorkletNode(this.ctx, 'cap');
      this.node.port.onmessage = e => this._onFrame(e.data);
      src.connect(this.node);
      // Выход worklet никуда не ведём: подключить его к destination
      // значило бы дать человеку услышать самого себя с задержкой.

      await this._setupEncoder();
      this.active = true;
      this.playAt = 0;
    }

    async _setupEncoder() {
      if (!global.AudioEncoder) return;         // нет WebCodecs — остаёмся на PCM
      try {
        const cfg = { codec: 'opus', sampleRate: SAMPLE_RATE,
                      numberOfChannels: 1, bitrate: 24000 };
        const ok = await AudioEncoder.isConfigSupported(cfg);
        if (!ok.supported) return;
        this.encoder = new AudioEncoder({
          output: chunk => {
            const b = new Uint8Array(chunk.byteLength);
            chunk.copyTo(b);
            this._send(b, CODEC_OPUS);
          },
          error: () => { this.encoder = null; this.codec = CODEC_PCM16; },
        });
        this.encoder.configure(cfg);
        this.codec = CODEC_OPUS;
      } catch (e) {
        this.encoder = null;
        this.codec = CODEC_PCM16;
      }
    }

    _onFrame(f32) {
      if (!this.active || this.muted) return;

      // Индикатор уровня: без него человек не понимает, слышно его
      // вообще или микрофон занят другой вкладкой.
      let peak = 0;
      for (let i = 0; i < f32.length; i++) peak = Math.max(peak, Math.abs(f32[i]));
      this.lastLevel = peak;
      if (this.on.onLevel) this.on.onLevel(peak);

      if (this.encoder && this.encoder.state === 'configured') {
        const data = new AudioData({
          format: 'f32-planar', sampleRate: SAMPLE_RATE,
          numberOfFrames: f32.length, numberOfChannels: 1,
          timestamp: (this.seq * FRAME_MS) * 1000, data: f32,
        });
        this.encoder.encode(data);
        data.close();
        return;
      }
      this._send(new Uint8Array(f32ToPcm16(f32).buffer), CODEC_PCM16);
    }

    _send(payload, codec) {
      if (!this.ws || this.ws.readyState !== 1) return;
      this.seq = (this.seq + 1) & 0xFFFF;
      this.ws.send(packAudio(payload, this.seq, codec, false));
    }

    // ── воспроизведение ──
    onBinary(buf) {
      const f = parseAudio(buf);
      if (!f) return;
      if (this.on.onSpeaker) this.on.onSpeaker(f.member, f.fromAir);
      if (f.codec === CODEC_OPUS) this._playOpus(f);
      else this._playPcm(new Int16Array(f.payload.buffer.slice(
        f.payload.byteOffset, f.payload.byteOffset + f.payload.byteLength)));
    }

    _ensureCtx() {
      if (!this.ctx) this.ctx = new AudioContext({ sampleRate: SAMPLE_RATE });
      return this.ctx;
    }

    _playPcm(pcm) {
      const ctx = this._ensureCtx();
      const f32 = pcm16ToF32(pcm);
      const buf = ctx.createBuffer(1, f32.length, SAMPLE_RATE);
      buf.copyToChannel(f32, 0);
      this._schedule(buf);
    }

    _playOpus(f) {
      if (!global.AudioDecoder) return;
      let dec = this.decoders.get(f.member);
      if (!dec) {
        dec = new AudioDecoder({
          output: data => {
            const f32 = new Float32Array(data.numberOfFrames);
            data.copyTo(f32, { planeIndex: 0, format: 'f32-planar' });
            const ctx = this._ensureCtx();
            const buf = ctx.createBuffer(1, f32.length, data.sampleRate);
            buf.copyToChannel(f32, 0);
            this._schedule(buf);
            data.close();
          },
          error: () => this.decoders.delete(f.member),
        });
        dec.configure({ codec: 'opus', sampleRate: SAMPLE_RATE,
                        numberOfChannels: 1 });
        this.decoders.set(f.member, dec);
      }
      try {
        dec.decode(new EncodedAudioChunk({
          type: 'key', timestamp: f.seq * FRAME_MS * 1000,
          data: f.payload,
        }));
      } catch (e) { this.decoders.delete(f.member); }
    }

    _schedule(buf) {
      const ctx = this._ensureCtx();
      const src = ctx.createBufferSource();
      src.buffer = buf;
      src.connect(ctx.destination);
      // Небольшой запас впереди текущего времени: без него каждый
      // сетевой всплеск слышен щелчком, а с большим — разговор
      // превращается в переписку.
      const now = ctx.currentTime + JITTER_MS / 1000;
      if (this.playAt < now) this.playAt = now;
      src.start(this.playAt);
      this.playAt += buf.duration;
    }

    setMuted(on) { this.muted = !!on; }

    stop() {
      this.active = false;
      if (this.node) { try { this.node.disconnect(); } catch (e) {} }
      if (this.stream) this.stream.getTracks().forEach(t => t.stop());
      if (this.encoder && this.encoder.state === 'configured') {
        try { this.encoder.close(); } catch (e) {}
      }
      this.decoders.forEach(d => { try { d.close(); } catch (e) {} });
      this.decoders.clear();
      this.encoder = null;
      this.stream = null;
      this.node = null;
      this.playAt = 0;
    }
  }

  global.LoraCall = { CallClient, parseAudio, packAudio, CODEC_OPUS, CODEC_PCM16 };
})(window);
