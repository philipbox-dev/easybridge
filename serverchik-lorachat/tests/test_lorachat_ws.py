"""Тесты WebSocket-канала на живом сервере.

Тестовый клиент Flask сокеты не умеет, поэтому здесь поднимается
настоящий сервер в потоке и к нему подключается настоящий клиент. Это
дороже обычных тестов, но иначе главный сценарий продукта — «сообщение
доходит в реальном времени» — не проверяется вообще.
"""

import json
import os
import socket
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

websocket = pytest.importorskip('websocket', reason='нужен websocket-client')
pytest.importorskip('flask_sock', reason='нужен flask-sock')


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


@pytest.fixture(scope='module')
def live(lc_app):
    """Живой сервер на общем приложении (см. tests/conftest.py).

    База чистится один раз на модуль, а не на каждый тест: сервер уже
    поднят, сокеты открыты, и дёргать drop_all под ними — верный способ
    получить плавающие падения.
    """
    from werkzeug.serving import make_server
    from lorachat_dev import seed, db, DevUser

    app = lc_app
    with app.app_context():
        db.drop_all()
        db.create_all()
        db.session.add(DevUser(id=1, username='host'))
        db.session.commit()
    seed(app)

    port = free_port()
    # threaded=True обязателен: тест держит несколько сокетов сразу, а
    # однопоточный сервер обслужил бы только первый и завис.
    srv = make_server('127.0.0.1', port, app, threaded=True)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    time.sleep(0.4)

    M = app.lc_models
    with app.app_context():
        members = {m.username: {'id': m.id, 'uid': m.uid, 'node': m.node_id}
                   for m in M['Member'].query.all()}

    yield {'app': app, 'port': port, 'members': members, 'url': 'http://127.0.0.1:%d' % port}
    srv.shutdown()


def phone_token(live, username, **kw):
    import urllib.request
    body = {'uid': live['members'][username]['uid'], 'password': 'test1234'}
    body.update(kw)
    req = urllib.request.Request(
        live['url'] + '/lora-chat/api/login',
        data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
    return json.load(urllib.request.urlopen(req, timeout=5))['token']


def connect(live, token, timeout=5):
    ws = websocket.create_connection(
        'ws://127.0.0.1:%d/lora-chat/api/ws?token=%s' % (live['port'], token),
        timeout=timeout)
    hello = json.loads(ws.recv())
    assert hello['op'] == 'hello_ok'
    return ws, hello


def drain(ws, op, tries=8, where=None):
    """Дождаться кадра нужного типа, пропуская presence и прочий шум.

    where — дополнительный фильтр: presence о собственном подключении
    приходит и самому подключившемуся, и его надо уметь пропустить.
    """
    for _ in range(tries):
        try:
            d = json.loads(ws.recv())
        except Exception:
            return None
        if d.get('op') == op and (where is None or where(d)):
            return d
    return None


def test_ws_rejects_bad_token(live):
    ws = websocket.create_connection(
        'ws://127.0.0.1:%d/lora-chat/api/ws?token=нетакой' % live['port'], timeout=5)
    d = json.loads(ws.recv())
    assert d['op'] == 'error' and d['code'] == 'auth'
    ws.close()


def test_ws_message_arrives_in_realtime(live):
    a_tok = phone_token(live, 'philip', can_gateway=True)
    b_tok = phone_token(live, 'misha', can_gateway=False)
    a, _ = connect(live, a_tok)
    b, _ = connect(live, b_tok)
    try:
        a.send(json.dumps({'op': 'msg', 'kind': 'text', 'text': 'на связи',
                           'ref': 1}))
        got = drain(b, 'msg')
        assert got is not None, 'сообщение не дошло по сокету'
        assert got['msg']['text'] == 'на связи'
        assert got['msg']['from'] == live['members']['philip']['id']
    finally:
        a.close(); b.close()


def test_ws_sender_gets_route_report(live):
    """Отправитель должен узнать, что часть адресатов доступна только
    по эфиру, — иначе он будет ждать ответа, которого не будет."""
    a_tok = phone_token(live, 'philip', can_gateway=True)
    b_tok = phone_token(live, 'misha')
    a, _ = connect(live, a_tok)
    b, _ = connect(live, b_tok)
    try:
        a.send(json.dumps({'op': 'msg', 'kind': 'text', 'text': 'кто где',
                           'ref': 2}))
        sent = drain(a, 'sent')
        assert sent is not None
        routes = sent['routes']
        misha = live['members']['misha']['id']
        roma = live['members']['roma']['id']
        assert misha in routes['web'], 'Миша в вебе — ему по интернету'
        assert roma in routes['lora'], 'Ромы нет в вебе — ему в эфир'
        assert routes['gateway'] is not None, 'телефон Philip должен стать шлюзом'
        assert routes['lora_blocked'] is False
    finally:
        a.close(); b.close()


def test_gateway_receives_air_command(live):
    """Тот самый сценарий: пишу из веба, Рома не в вебе — его телефон
    должен получить команду вынести сообщение в эфир с пометкой WEB."""
    gw_tok = phone_token(live, 'philip', can_gateway=True)
    gw, _ = connect(live, gw_tok)
    sender_tok = phone_token(live, 'misha', can_gateway=False)
    sender, _ = connect(live, sender_tok)
    try:
        sender.send(json.dumps({'op': 'msg', 'kind': 'text',
                                'text': 'Рома, ответь', 'ref': 3}))
        cmd = drain(gw, 'lora_tx', tries=12)
        assert cmd is not None, 'шлюз не получил команду в эфир'
        assert cmd['web_flag'] is True
        assert cmd['text'] == 'Рома, ответь'
        nodes = [t['node'] for t in cmd['targets']]
        assert live['members']['roma']['node'] in nodes
    finally:
        gw.close(); sender.close()


def test_no_gateway_is_reported_honestly(live):
    """Если ни один телефон не готов быть шлюзом, отправитель должен
    узнать, что часть людей сообщения не получит."""
    tok = phone_token(live, 'misha', can_gateway=False)
    ws, _ = connect(live, tok)
    try:
        ws.send(json.dumps({'op': 'msg', 'kind': 'text', 'text': 'эй', 'ref': 4}))
        sent = drain(ws, 'sent')
        assert sent['routes']['lora_blocked'] is True
        assert sent['routes']['gateway'] is None
    finally:
        ws.close()


def test_sos_reaches_everyone_both_ways(live):
    gw_tok = phone_token(live, 'philip', can_gateway=True)
    gw, _ = connect(live, gw_tok)
    b_tok = phone_token(live, 'misha')
    b, _ = connect(live, b_tok)
    try:
        b.send(json.dumps({'op': 'msg', 'kind': 'sos', 'text': 'нога', 'ref': 5}))
        got = drain(gw, 'msg', tries=12)
        assert got is not None and got['msg']['kind'] == 'sos'
        sent = drain(b, 'sent', tries=12)
        if sent:
            roma = live['members']['roma']['id']
            philip = live['members']['philip']['id']
            assert roma in sent['routes']['lora']
            # SOS дублируется в эфир даже тем, кто уже получил его в вебе
            assert philip in sent['routes']['lora']
    finally:
        gw.close(); b.close()


def test_presence_broadcast(live):
    a_tok = phone_token(live, 'philip')
    a, _ = connect(live, a_tok)
    try:
        b_tok = phone_token(live, 'roma')
        b, _ = connect(live, b_tok)
        # Своё собственное подключение прилетает первым — ждём именно
        # событие про Рому.
        roma_id = live['members']['roma']['id']
        ev = drain(a, 'presence', tries=8, where=lambda d: d['member'] == roma_id)
        assert ev is not None and ev['online'] is True
        b.close()
    finally:
        a.close()


def test_ping_pong(live):
    tok = phone_token(live, 'philip')
    ws, _ = connect(live, tok)
    try:
        ws.send(json.dumps({'op': 'ping', 't': 123}))
        assert drain(ws, 'pong')['t'] == 123
    finally:
        ws.close()


def test_unknown_op_is_reported(live):
    tok = phone_token(live, 'philip')
    ws, _ = connect(live, tok)
    try:
        ws.send(json.dumps({'op': 'станцевать'}))
        d = drain(ws, 'error')
        assert d['code'] == 'unknown_op'
    finally:
        ws.close()


def test_bad_json_does_not_kill_socket(live):
    tok = phone_token(live, 'philip')
    ws, _ = connect(live, tok)
    try:
        ws.send('{это не json')
        assert drain(ws, 'error')['code'] == 'bad_json'
        ws.send(json.dumps({'op': 'ping', 't': 7}))
        assert drain(ws, 'pong')['t'] == 7, 'сокет должен пережить мусор'
    finally:
        ws.close()


# ═══════════════════════════════════════════
#          Звонки: два плеча разом
# ═══════════════════════════════════════════
from lorachat_call import CODEC_OPUS, pack_audio, parse_audio  # noqa: E402


def drain_bin(ws, tries=8):
    """Дождаться бинарного (аудио) кадра, пропуская служебный JSON."""
    for _ in range(tries):
        try:
            frame = ws.recv()
        except Exception:
            return None
        if isinstance(frame, (bytes, bytearray)):
            got = parse_audio(frame)
            if got:
                return got
    return None


@pytest.fixture
def call_cleanup(live):
    """После каждого теста звонок надо снести: он живёт на всю сеть, и
    следующий тест иначе присоединится к чужому разговору."""
    yield
    app = live['app']
    with app.app_context():
        pass
    tok = phone_token(live, 'philip')
    ws, _ = connect(live, tok)
    try:
        ws.send(json.dumps({'op': 'call_leave'}))
        time.sleep(0.1)
        ws.send(json.dumps({'op': 'call_end'}))
        time.sleep(0.1)
    finally:
        ws.close()


def test_call_start_reports_air_participants(live, call_cleanup):
    """Тот самый сценарий: я и Миша в вебе, Рома — только в эфире.

    Звонок обязан увидеть Рому и поднять радио-плечо, а не сделать
    вид, что участников двое.
    """
    gw_tok = phone_token(live, 'philip', can_gateway=True, has_fsk=True)
    gw, _ = connect(live, gw_tok)
    misha, _ = connect(live, phone_token(live, 'misha'))
    try:
        gw.send(json.dumps({'op': 'call_start', 'codec': CODEC_OPUS}))
        st = drain(gw, 'call_state')
        assert st is not None
        assert st['bridged'] is True, 'Рома в эфире — плечо обязано подняться'
        nodes = [a['node'] for a in st['air']]
        assert live['members']['roma']['node'] in nodes
        assert st.get('note') is None, 'шлюз есть, жаловаться не на что'
    finally:
        gw.close(); misha.close()


def test_gateway_gets_air_start_command(live, call_cleanup):
    """Телефон-шлюз должен получить команду поднять FSK и список тех,
    кого звать в эфире."""
    gw_tok = phone_token(live, 'philip', can_gateway=True, has_fsk=True)
    gw, _ = connect(live, gw_tok)
    try:
        gw.send(json.dumps({'op': 'call_start', 'mode': 1}))
        cmd = drain(gw, 'call_air_start', tries=10)
        assert cmd is not None, 'шлюз не получил команду на FSK'
        assert cmd['air_codec'] == 'amr-nb'
        # Не opus: как только появляется эфирное плечо, весь звонок
        # переводится на PCM16 — мост на телефоне пережимает его в
        # AMR-NB, а Opus в MediaCodec требует codec-specific data,
        # которых у потока из браузера нет.
        assert cmd['web_codec'] == 'pcm16'
        nodes = [t['node'] for t in cmd['targets']]
        assert live['members']['roma']['node'] in nodes
    finally:
        gw.close()


def test_bridged_call_switches_everyone_to_pcm(live, call_cleanup):
    """Браузеры должны узнать о смене кодека, иначе продолжат слать
    Opus — и в эфир не уйдёт ничего, без единой ошибки в логах."""
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    web, _ = connect(live, phone_token(live, 'misha'))
    try:
        gw.send(json.dumps({'op': 'call_start'}))
        st = drain(gw, 'call_state', tries=10)
        assert st is not None and st['bridged'] is True
        assert st['codec'] == 0, 'PCM16 (0) — единственный, что понимает мост'
    finally:
        gw.close(); web.close()


def test_pure_web_call_keeps_opus(live, call_cleanup):
    """Без эфирного плеча пережимать нечего — оставляем Opus, он вдвое
    экономнее по трафику."""
    a, _ = connect(live, phone_token(live, 'philip', can_gateway=False))
    b, _ = connect(live, phone_token(live, 'misha', can_gateway=False))
    c, _ = connect(live, phone_token(live, 'roma', can_gateway=False))
    try:
        a.send(json.dumps({'op': 'call_start', 'codec': 1}))
        drain(a, 'call_state', tries=10)
        b.send(json.dumps({'op': 'call_join', 'codec': 1}))
        c.send(json.dumps({'op': 'call_join', 'codec': 1}))
        st = drain(c, 'call_state', tries=10)
        assert st is not None
        # все трое в вебе → эфирного плеча нет
        assert st['air'] == []
        assert st['codec'] == 1
    finally:
        a.close(); b.close(); c.close()


def test_call_without_gateway_says_so(live, call_cleanup):
    """Некому вынести голос в эфир — звонящий должен это узнать сразу,
    а не гадать, почему Рома молчит."""
    ws, _ = connect(live, phone_token(live, 'misha', can_gateway=False))
    try:
        ws.send(json.dumps({'op': 'call_start'}))
        st = drain(ws, 'call_state')
        assert st is not None
        assert st['note'] and 'шлюз' in st['note']
        assert st['gateway'] is None
    finally:
        ws.close()


def test_gateway_without_fsk_is_refused(live, call_cleanup):
    """Телефон подключён к плате без FSK (E220): текст она передаст,
    голос — нет. Молча считать её шлюзом нельзя."""
    ws, _ = connect(live, phone_token(live, 'philip', can_gateway=True,
                                      has_fsk=False, hw_profile='c6_e220'))
    try:
        ws.send(json.dumps({'op': 'call_start'}))
        st = drain(ws, 'call_state')
        assert st is not None
        assert st['note'] and 'FSK' in st['note']
        assert st['gateway'] is None
    finally:
        ws.close()


def test_audio_reaches_other_web_participant(live, call_cleanup):
    a, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        a.send(json.dumps({'op': 'call_start'}))
        drain(a, 'call_state')
        b.send(json.dumps({'op': 'call_join'}))
        drain(b, 'call_state')

        a.send_binary(pack_audio(b'\xde\xad\xbe\xef', 0, 1, CODEC_OPUS))
        got = drain_bin(b, tries=10)
        assert got is not None, 'звук не дошёл до второго участника'
        assert got['payload'] == b'\xde\xad\xbe\xef'
        assert got['member'] == live['members']['philip']['id']
        assert got['from_air'] is False
    finally:
        a.close(); b.close()


def test_speaker_does_not_hear_himself(live, call_cleanup):
    a, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        a.send(json.dumps({'op': 'call_start'}))
        drain(a, 'call_state')
        b.send(json.dumps({'op': 'call_join'}))
        drain(b, 'call_state')
        a.send_binary(pack_audio(b'\x01\x02', 0, 5, CODEC_OPUS))
        time.sleep(0.3)
        a.settimeout(0.5)
        echo = drain_bin(a, tries=3)
        assert echo is None, 'эхо собственного голоса — худшее, что можно сделать'
    finally:
        a.close(); b.close()


def test_air_audio_only_from_gateway(live, call_cleanup):
    """Кадр «из эфира» вправе прислать только шлюз. Иначе любой
    участник подделает голос человека, которого в вебе нет."""
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        gw.send(json.dumps({'op': 'call_start'}))
        drain(gw, 'call_state')
        b.send(json.dumps({'op': 'call_join'}))
        drain(b, 'call_state')

        # Миша выдаёт себя за эфир
        b.send_binary(pack_audio(b'\xff\xff', 0, 1, CODEC_OPUS, from_air=True))
        time.sleep(0.3)
        gw.settimeout(0.6)
        assert drain_bin(gw, tries=3) is None, 'подделка эфира прошла'
    finally:
        gw.close(); b.close()


def test_air_audio_from_gateway_reaches_web(live, call_cleanup):
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        gw.send(json.dumps({'op': 'call_start'}))
        drain(gw, 'call_state')
        b.send(json.dumps({'op': 'call_join'}))
        drain(b, 'call_state')

        gw.send_binary(pack_audio(b'\x10\x20', 0, 3, CODEC_OPUS, from_air=True))
        got = drain_bin(b, tries=10)
        assert got is not None and got['from_air'] is True
        assert got['payload'] == b'\x10\x20'
    finally:
        gw.close(); b.close()


def test_floor_is_granted_and_denied(live, call_cleanup):
    a, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        a.send(json.dumps({'op': 'call_start'}))
        drain(a, 'call_state')
        b.send(json.dumps({'op': 'call_join'}))
        drain(b, 'call_state')

        a.send(json.dumps({'op': 'call_floor', 'take': True}))
        mine = drain(a, 'call_floor')
        assert mine['granted'] is True

        b.send(json.dumps({'op': 'call_floor', 'take': True}))
        theirs = drain(b, 'call_floor', tries=10,
                       where=lambda d: d['granted'] is False)
        assert theirs is not None, 'второму слово выдавать нельзя'
        assert theirs['holder'] == live['members']['philip']['id']
    finally:
        a.close(); b.close()


def test_call_ends_when_last_participant_drops(live, call_cleanup):
    a, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        a.send(json.dumps({'op': 'call_start'}))
        drain(a, 'call_state')
        b.send(json.dumps({'op': 'call_join'}))
        drain(b, 'call_state')

        a.send(json.dumps({'op': 'call_leave'}))
        drain(a, 'call_state', tries=4)
        b.send(json.dumps({'op': 'call_leave'}))
        assert drain(b, 'call_end', tries=6) is not None
    finally:
        a.close(); b.close()


def test_invite_reaches_the_whole_network(live, call_cleanup):
    """Не участвующие в звонке тоже должны узнать, что он идёт."""
    a, _ = connect(live, phone_token(live, 'philip', can_gateway=True, has_fsk=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        a.send(json.dumps({'op': 'call_start'}))
        inv = drain(b, 'call_invite', tries=10)
        assert inv is not None
        assert inv['from'] == live['members']['philip']['id']
        assert inv['from_name'] == 'Philip'
    finally:
        a.close(); b.close()


# ═══════════════════════════════════════════
#     Автономные устройства через сокет
# ═══════════════════════════════════════════
MANIFEST = {
    'op': 'dev_hello', 'node_id': '0xBEEF0042', 'name': 'Метеостанция',
    'class': 0, 'flags': 1, 'interval': 60,
    'fields': [{'i': 0, 'name': 'temp', 'type': 0, 'unit': '°C', 'scale': -1},
               {'i': 1, 'name': 'rain', 'type': 1, 'unit': 'мм', 'scale': 0}],
    'cmds': [{'id': 1, 'name': 'Реле', 'action': 1}],
}


def test_manifest_creates_device_and_reaches_web(live):
    """Датчик объявился в эфире — виджет появляется у всех в вебе."""
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True))
    watcher, _ = connect(live, phone_token(live, 'misha'))
    try:
        gw.send(json.dumps(MANIFEST))
        ev = drain(watcher, 'dev_hello', tries=10)
        assert ev is not None, 'манифест не доехал до веба'
        d = ev['device']
        assert d['node_id'] == '0xBEEF0042'
        assert d['name'] == 'Метеостанция'
        assert [f['name'] for f in d['fields']] == ['temp', 'rain']
        assert d['cmds'][0]['name'] == 'Реле'
    finally:
        gw.close(); watcher.close()


def test_readings_flow_to_web(live):
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True))
    watcher, _ = connect(live, phone_token(live, 'misha'))
    try:
        gw.send(json.dumps(MANIFEST))
        drain(watcher, 'dev_hello', tries=10)
        gw.send(json.dumps({'op': 'dev_data', 'node_id': '0xBEEF0042',
                            'rssi': -97,
                            'v': [{'i': 0, 't': 0, 'v': 187},
                                  {'i': 1, 't': 1, 'v': 0}]}))
        ev = drain(watcher, 'dev_data', tries=10)
        assert ev is not None
        assert ev['values'] == {'0': 187, '1': 0}
        assert ev['rssi'] == -97
    finally:
        gw.close(); watcher.close()


def test_browser_cannot_pretend_to_be_a_sensor(live):
    """У вкладки браузера радио нет. Принимать от неё показания значило
    бы разрешить рисовать любые числа от имени чужого датчика."""
    ws, _ = connect(live, phone_token(live, 'misha'))
    try:
        # логинимся как телефон, но объявляем себя вкладкой
        import urllib.request
        req = urllib.request.Request(
            live['url'] + '/lora-chat/api/login',
            data=json.dumps({'uid': live['members']['roma']['uid'],
                             'password': 'test1234'}).encode(),
            headers={'Content-Type': 'application/json'})
        json.load(urllib.request.urlopen(req, timeout=5))
    finally:
        ws.close()

    # браузерная сессия (кука, а не токен телефона) — kind='web'
    import urllib.request
    import http.cookiejar
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    opener.open(live['url'] + '/lora-chat/%s/login'
                % live['members']['roma']['uid'],
                data=b'password=test1234', timeout=5)
    cookies = '; '.join('%s=%s' % (c.name, c.value) for c in jar)
    wsb = websocket.create_connection(
        'ws://127.0.0.1:%d/lora-chat/api/ws' % live['port'],
        header=['Cookie: ' + cookies], timeout=5)
    try:
        hello = json.loads(wsb.recv())
        assert hello['op'] == 'hello_ok' and hello['kind'] == 'web'
        wsb.send(json.dumps(dict(MANIFEST, node_id='0xFAKE0001')))
        err = drain(wsb, 'error', tries=6)
        assert err is not None and err['code'] == 'not_a_gateway'
    finally:
        wsb.close()


def test_command_needs_a_gateway(live):
    """Команда на железо без шлюза невозможна — и это надо сказать."""
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True))
    gw.send(json.dumps(MANIFEST))
    time.sleep(0.3)
    gw.close()
    time.sleep(0.3)

    lonely, _ = connect(live, phone_token(live, 'misha', can_gateway=False))
    try:
        lonely.send(json.dumps({'op': 'dev_cmd', 'node_id': '0xBEEF0042',
                                'id': 1, 'arg': 1}))
        err = drain(lonely, 'error', tries=8)
        assert err is not None and 'шлюз' in err['desc']
    finally:
        lonely.close()


def test_command_reaches_the_gateway(live):
    """Человек за компом жмёт кнопку на виджете, а в эфир команду
    выносит чужой телефон — свой у него шлюзом не работает."""
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True))
    web, _ = connect(live, phone_token(live, 'misha', can_gateway=False))
    try:
        gw.send(json.dumps(MANIFEST))
        drain(web, 'dev_hello', tries=10)
        web.send(json.dumps({'op': 'dev_cmd', 'node_id': '0xBEEF0042',
                             'id': 1, 'arg': 1}))
        cmd = drain(gw, 'dev_tx', tries=12)
        assert cmd is not None, 'шлюз не получил команду для устройства'
        assert cmd['node_id'] == '0xBEEF0042'
        assert cmd['id'] == 1 and cmd['arg'] == 1
        assert cmd['by'] == 'Misha', 'в команде должно быть видно, кто её нажал'
    finally:
        gw.close(); web.close()


def test_unknown_command_is_refused(live):
    gw, _ = connect(live, phone_token(live, 'philip', can_gateway=True))
    try:
        gw.send(json.dumps(MANIFEST))
        time.sleep(0.3)
        gw.send(json.dumps({'op': 'dev_cmd', 'node_id': '0xBEEF0042',
                            'id': 99, 'arg': 1}))
        err = drain(gw, 'error', tries=8)
        assert err is not None and 'команды' in err['desc']
    finally:
        gw.close()


def test_button_sends_message(live):
    """Кнопка-плагин: одно нажатие — сообщение всем."""
    app = live['app']
    M = app.lc_models
    from lorachat_dev import db
    with app.app_context():
        net = M['Network'].query.first()
        btn = M['Button'](network_id=net.id, label='Сбор', kind='message',
                          text='Всем сбор у моста')
        db.session.add(btn)
        db.session.commit()
        bid = btn.id

    a, _ = connect(live, phone_token(live, 'philip', can_gateway=True))
    b, _ = connect(live, phone_token(live, 'misha'))
    try:
        a.send(json.dumps({'op': 'button', 'id': bid}))
        got = drain(b, 'msg', tries=10)
        assert got is not None
        assert got['msg']['text'] == 'Всем сбор у моста'
    finally:
        a.close(); b.close()


def test_host_only_button_refused_for_others(live):
    app = live['app']
    M = app.lc_models
    from lorachat_dev import db
    with app.app_context():
        net = M['Network'].query.first()
        btn = M['Button'](network_id=net.id, label='Тревога', kind='sos',
                          text='ТРЕВОГА', host_only=True)
        db.session.add(btn)
        db.session.commit()
        bid = btn.id

    ws, _ = connect(live, phone_token(live, 'misha'))
    try:
        ws.send(json.dumps({'op': 'button', 'id': bid}))
        err = drain(ws, 'error', tries=8)
        assert err is not None and 'хоста' in err['desc']
    finally:
        ws.close()
