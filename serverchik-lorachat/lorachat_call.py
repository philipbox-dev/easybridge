"""
Звонки EasyBridge: одно плечо в вебе, второе — в эфире.

Задача, ради которой всё это существует: **в одном разговоре
участвуют люди с интернетом и люди без него**. Ты и Миша сидят за
компами, Рома в лесу с рацией — и все трое должны слышать друг друга.

Как это устроено:

    браузер ──opus──┐                        ┌── браузер
                    ├── сервер (этот файл) ──┤
    браузер ──opus──┘         │              └── браузер
                              │ только речь того,
                              │ у кого сейчас слово
                              ▼
                     телефон-шлюз (перекодирует)
                              │ AMR-NB по BLE
                              ▼
                       ESP32 → GFSK → эфир → Рома

Три вещи, которые определяют весь дизайн:

1. **Сервер не трогает звук.** Он раздаёт кадры как есть, не зная, что
   внутри. Перекодированием занимается телефон-шлюз: у Android уже
   есть и Opus, и AMR-NB, а тащить кодеки на веб-сервер — значит
   поставить туда ffmpeg и следить за его загрузкой на каждом звонке.

2. **Эфир полудуплексный, веб — нет.** По FSK в один момент говорит
   ровно один. Поэтому у радио-плеча есть «слово» (floor): веб-участники
   могут перебивать друг друга сколько угодно, но в эфир уйдёт только
   тот, кто это слово держит. Без такого арбитража два одновременных
   говорящих превратились бы в кашу на 4.75 кбит/с.

3. **Звонок не обязан иметь оба плеча.** Если в эфире никого нет —
   это обычная веб-конференция. Если шлюза нет, а участники в эфире
   есть — мы обязаны сказать об этом вслух, а не молча их потерять.
"""

import threading
import time

# Маркер бинарного аудиокадра. Звук ходит бинарём, а не JSON+base64:
# кадр Opus весит ~60 байт, и обвязка base64 раздула бы поток на треть
# при 50 кадрах в секунду.
AUDIO_MAGIC = 0xA1
AUDIO_HEADER = 8

CODEC_PCM16 = 0        # 16 кГц моно, little-endian — если нет WebCodecs
CODEC_OPUS = 1         # 48 кГц моно, по кадру на пакет
CODEC_AMR_NB = 2       # то, что реально летит в эфир (шлюз отдаёт его нам)
CODEC_NAMES = {CODEC_PCM16: 'pcm16', CODEC_OPUS: 'opus', CODEC_AMR_NB: 'amr-nb'}

# Сколько молчания держим слово за говорящим, прежде чем отдать его
# другому. Меньше — и слово будет выхватывать любой шорох между фразами.
FLOOR_HOLD_MS = 1200
# Звонок без единого кадра и без участников умирает сам: браузер могли
# закрыть, не нажав «положить трубку».
CALL_IDLE_TIMEOUT_S = 90


def pack_audio(payload, member_id, seq, codec, from_air=False):
    """Собрать бинарный кадр для отправки клиенту."""
    head = bytearray(AUDIO_HEADER)
    head[0] = AUDIO_MAGIC
    head[1] = 0x01 if from_air else 0x00
    head[2] = (member_id >> 8) & 0xFF
    head[3] = member_id & 0xFF
    head[4] = (seq >> 8) & 0xFF
    head[5] = seq & 0xFF
    head[6] = codec & 0xFF
    head[7] = 0
    return bytes(head) + payload


def parse_audio(data):
    """Разобрать входящий кадр. None, если это не аудио."""
    if not isinstance(data, (bytes, bytearray)) or len(data) < AUDIO_HEADER:
        return None
    if data[0] != AUDIO_MAGIC:
        return None
    return {
        'from_air': bool(data[1] & 0x01),
        'member': (data[2] << 8) | data[3],
        'seq': (data[4] << 8) | data[5],
        'codec': data[6],
        'payload': bytes(data[AUDIO_HEADER:]),
    }


class Call:
    """Один разговор внутри одной сети."""

    def __init__(self, call_id, network_id, starter_member, mode=1):
        self.id = call_id
        self.network_id = network_id
        self.starter = starter_member
        self.mode = mode                  # FSK_PROFILE_*: 0 дальнобой, 1 станд., 2 HD
        self.created_at = time.time()
        self.last_audio_at = time.time()
        self.ended = False

        # member_id -> {'device': id, 'joined': ts, 'codec': int, 'muted': bool}
        self.web = {}
        # Узлы, которых слышно только по эфиру: node_id -> {'name', 'last'}
        self.air = {}

        # Кодек, на котором говорят веб-участники. Пока звонок чисто
        # веб-овый — Opus (вдвое меньше трафика). Как только появляется
        # эфирное плечо, переводим всех на PCM16: мост на телефоне
        # пережимает в AMR-NB, а сырой PCM для этого нужен без плясок с
        # codec-specific data, которых требует Opus в MediaCodec.
        self.codec = CODEC_OPUS

        # Кто сейчас говорит В ЭФИР. None — слово свободно.
        self.floor = None
        self.floor_since = 0.0
        self.floor_last_frame = 0.0
        self.gateway_device = None

    # ── участники ──
    def join_web(self, member_id, device_id, codec):
        self.web[member_id] = {'device': device_id, 'joined': time.time(),
                               'codec': codec, 'muted': False}

    def leave_web(self, member_id):
        self.web.pop(member_id, None)
        if self.floor == member_id:
            self.release_floor(member_id)

    def note_air_node(self, node_id, name=None):
        entry = self.air.setdefault(node_id, {'name': name or node_id})
        entry['last'] = time.time()
        if name:
            entry['name'] = name

    def is_empty(self):
        """Звонок существует, пока есть кого мостить.

        Раньше здесь дополнительно проверялись узлы в эфире — и звонок
        жил вечно после ухода последнего веб-участника. Это была
        ошибка: наш объект нужен только для склейки веб↔эфир. Ушли все
        из веба — склеивать нечего, шлюзу говорим отпустить FSK. Люди
        в эфире при этом не теряют связь: они продолжают говорить
        рацией друг с другом, просто уже без нас.
        """
        return not self.web

    # ── слово в эфире ──
    def floor_free(self, now=None):
        """Свободно ли слово. Держится ещё FLOOR_HOLD_MS после последнего
        кадра — иначе пауза между словами отдавала бы микрофон соседу."""
        if self.floor is None:
            return True
        now = now or time.time()
        return (now - self.floor_last_frame) * 1000 > FLOOR_HOLD_MS

    def take_floor(self, member_id, now=None):
        now = now or time.time()
        if self.floor == member_id:
            self.floor_last_frame = now
            return True
        if not self.floor_free(now):
            return False
        self.floor = member_id
        self.floor_since = now
        self.floor_last_frame = now
        return True

    def release_floor(self, member_id):
        if self.floor == member_id:
            self.floor = None
            self.floor_since = 0.0
            return True
        return False

    def state(self):
        return {
            'call': self.id,
            'mode': self.mode,
            'starter': self.starter,
            'web': sorted(self.web.keys()),
            'air': [{'node': n, 'name': v.get('name')} for n, v in self.air.items()],
            'floor': self.floor,
            'codec': self.codec,
            'gateway': self.gateway_device,
            'bridged': bool(self.air) or self.gateway_device is not None,
        }


class CallManager:
    """Все активные звонки. Как и шина, ничего не знает про Flask."""

    def __init__(self):
        self._lock = threading.RLock()
        self._calls = {}          # call_id -> Call
        self._by_net = {}         # network_id -> call_id
        self._seq = 0

    def _next_id(self):
        self._seq += 1
        return 'c%d-%d' % (int(time.time()), self._seq)

    def get(self, call_id):
        with self._lock:
            return self._calls.get(call_id)

    def for_network(self, network_id):
        """Активный звонок сети.

        Звонок на сеть один: LoRa-канал физически один, и два
        одновременных разговора в эфире всё равно не разъедутся. Кто
        позвонил вторым — присоединяется к первому.
        """
        with self._lock:
            cid = self._by_net.get(network_id)
            return self._calls.get(cid) if cid else None

    def start(self, network_id, member_id, device_id, mode=1, codec=CODEC_OPUS):
        with self._lock:
            existing = self.for_network(network_id)
            if existing and not existing.ended:
                existing.join_web(member_id, device_id, codec)
                return existing, False
            call = Call(self._next_id(), network_id, member_id, mode)
            call.join_web(member_id, device_id, codec)
            self._calls[call.id] = call
            self._by_net[network_id] = call.id
            return call, True

    def join(self, network_id, member_id, device_id, codec=CODEC_OPUS):
        with self._lock:
            call = self.for_network(network_id)
            if not call or call.ended:
                return None
            call.join_web(member_id, device_id, codec)
            return call

    def leave(self, network_id, member_id):
        with self._lock:
            call = self.for_network(network_id)
            if not call:
                return None, False
            call.leave_web(member_id)
            if call.is_empty():
                self.end(network_id)
                return call, True
            return call, False

    def end(self, network_id):
        with self._lock:
            cid = self._by_net.pop(network_id, None)
            call = self._calls.pop(cid, None) if cid else None
            if call:
                call.ended = True
            return call

    def sweep(self, now=None):
        """Прибрать звонки, из которых все ушли молча."""
        now = now or time.time()
        dead = []
        with self._lock:
            for cid, call in list(self._calls.items()):
                idle = now - max(call.last_audio_at, call.created_at)
                if call.is_empty() or idle > CALL_IDLE_TIMEOUT_S:
                    self._calls.pop(cid, None)
                    if self._by_net.get(call.network_id) == cid:
                        self._by_net.pop(call.network_id, None)
                    call.ended = True
                    dead.append(call)
        return dead

    def stats(self):
        with self._lock:
            return {'calls': len(self._calls),
                    'participants': sum(len(c.web) for c in self._calls.values())}
