"""Тесты LoRa-чата.

Главное, что здесь проверяется, — **маршрутизация**: сообщение обязано
уйти по интернету тем, кто в вебе, и по эфиру тем, кого там нет. Это
единственное место, где ошибка не видна глазом: чат выглядит рабочим,
а человек в лесу просто не получает сообщений.

Запуск:  pytest tests/test_lorachat.py -q
"""

import os
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lorachat_bus import Conn, Hub          # noqa: E402


# ═══════════════════════════════════════════
#              Шина и маршруты
# ═══════════════════════════════════════════
class FakeWS:
    """Сокет, который всё записывает вместо отправки."""

    def __init__(self, broken=False):
        self.sent = []
        self.broken = broken
        self.closed = False

    def send(self, data):
        if self.broken:
            raise ConnectionError('порвалось')
        self.sent.append(data)

    def close(self):
        self.closed = True


def mkconn(hub, member_id, device_id, kind='web', gateway=False, net=1):
    c = Conn(FakeWS(), net, member_id, device_id, kind, gateway)
    hub.add(c)
    return c


def test_plan_web_only_when_everyone_online():
    hub = Hub()
    mkconn(hub, member_id=2, device_id=20)
    mkconn(hub, member_id=3, device_id=30)
    web, lora = hub.plan(1, [2, 3], sender_member_id=1, kind='text')
    assert web == {2, 3}
    assert lora == set(), 'эфир не нужен, если все в вебе'


def test_plan_falls_back_to_air_for_offline():
    """Сценарий из ТЗ: я и Миша в вебе, Ромы нет.

    Мише сообщение идёт по интернету, Роме — обязано уйти по эфиру.
    """
    hub = Hub()
    mkconn(hub, member_id=1, device_id=10)     # я
    mkconn(hub, member_id=2, device_id=20)     # Миша
    web, lora = hub.plan(1, [2, 3], sender_member_id=1, kind='text')
    assert web == {2}
    assert lora == {3}, 'Рома не в вебе — только эфир'


def test_sos_goes_both_ways():
    """SOS дублируется всеми маршрутами, даже когда веба достаточно."""
    hub = Hub()
    mkconn(hub, member_id=1, device_id=10)
    mkconn(hub, member_id=2, device_id=20)
    web, lora = hub.plan(1, [2, 3], sender_member_id=1, kind='sos')
    assert 2 in web
    assert lora == {2, 3}, 'SOS уходит в эфир всем, включая онлайновых'


def test_gateway_prefers_sender_own_phone():
    hub = Hub()
    mkconn(hub, member_id=1, device_id=11, kind='phone', gateway=True)
    mkconn(hub, member_id=2, device_id=22, kind='phone', gateway=True)
    gw = hub.pick_gateway(1, prefer_member_id=1)
    assert gw.device_id == 11, 'свой телефон ближе всех к отправителю'


def test_gateway_falls_back_to_any_phone():
    hub = Hub()
    mkconn(hub, member_id=2, device_id=22, kind='phone', gateway=True)
    gw = hub.pick_gateway(1, prefer_member_id=1)
    assert gw.device_id == 22


def test_browser_is_never_a_gateway():
    """Вкладка браузера в эфир не умеет — у неё нет радио."""
    hub = Hub()
    mkconn(hub, member_id=2, device_id=22, kind='web', gateway=True)
    assert hub.pick_gateway(1) is None


def test_phone_can_refuse_gateway_duty():
    hub = Hub()
    c = mkconn(hub, member_id=2, device_id=22, kind='phone', gateway=True)
    assert hub.pick_gateway(1) is not None
    c.can_gateway = False              # роуминг, мало заряда
    assert hub.pick_gateway(1) is None


def test_broken_socket_marked_dead_and_swept():
    hub = Hub()
    c = Conn(FakeWS(broken=True), 1, 2, 20, 'web', False)
    hub.add(c)
    assert hub.is_online(1, 2)
    assert c.send({'op': 'ping'}) is False
    assert c.alive is False
    hub.sweep()
    assert not hub.is_online(1, 2), 'мёртвое соединение должно уйти из реестра'


def test_dead_conn_does_not_count_as_online():
    hub = Hub()
    c = mkconn(hub, member_id=2, device_id=20)
    c.alive = False
    web, lora = hub.plan(1, [2], sender_member_id=1, kind='text')
    assert lora == {2}, 'отвалившийся веб — повод идти в эфир'


# ═══════════════════════════════════════════
#            Приложение целиком
# ═══════════════════════════════════════════
# Приложение и очистка базы — общие для обоих файлов тестов, см.
# tests/conftest.py (фабрика блюпринта объявляет модели SQLAlchemy и
# может быть вызвана только один раз за процесс).
@pytest.fixture
def app(lc_clean):
    return lc_clean


@pytest.fixture
def members(app):
    M = app.lc_models
    with app.app_context():
        rows = M['Member'].query.order_by(M['Member'].id).all()
        return {m.username: {'id': m.id, 'uid': m.uid, 'node': m.node_id}
                for m in rows}


def login(client, uid, password='test1234'):
    return client.post('/lora-chat/%s/login' % uid, data={'password': password},
                       follow_redirects=False)


def test_link_alone_is_not_enough(app, members):
    """Персональная ссылка попадёт в историю браузера и в чужой скриншот,
    поэтому сама по себе пропуском быть не должна."""
    c = app.test_client()
    r = c.get('/lora-chat/%s/chat' % members['misha']['uid'])
    assert r.status_code == 200
    assert 'Пароль'.encode() in r.data
    assert 'Сообщение'.encode() not in r.data


def test_wrong_password_rejected(app, members):
    c = app.test_client()
    r = c.post('/lora-chat/%s/login' % members['misha']['uid'],
               data={'password': 'нет'})
    assert r.status_code == 401


def test_login_opens_the_room(app, members):
    c = app.test_client()
    assert login(c, members['misha']['uid']).status_code == 302
    r = c.get('/lora-chat/%s/chat' % members['misha']['uid'])
    assert r.status_code == 200
    assert 'Участники'.encode() in r.data


def test_member_cannot_open_someone_elses_chat(app, members):
    c = app.test_client()
    login(c, members['misha']['uid'])
    # Вход как Миша не должен открывать комнату Ромы.
    r = c.get('/lora-chat/%s/chat' % members['roma']['uid'])
    assert 'Пароль'.encode() in r.data


def test_send_and_history_roundtrip(app, members):
    c = app.test_client()
    login(c, members['misha']['uid'])
    r = c.post('/lora-chat/api/send', json={'kind': 'text', 'text': 'привет'})
    assert r.status_code == 200 and r.get_json()['ok']

    h = c.get('/lora-chat/api/history').get_json()
    assert h['ok']
    assert [m['text'] for m in h['messages']] == ['привет']
    assert h['messages'][0]['origin'] == 'web'


def test_empty_message_rejected(app, members):
    c = app.test_client()
    login(c, members['misha']['uid'])
    r = c.post('/lora-chat/api/send', json={'kind': 'text', 'text': '   '})
    assert r.status_code == 400


def test_history_needs_auth(app):
    c = app.test_client()
    assert c.get('/lora-chat/api/history').status_code == 401


def test_text_is_encrypted_at_rest(app, members):
    """На диске не должно лежать читаемого текста: смысл шифрования
    здесь — унесённый бэкап, а не защита от сервера."""
    c = app.test_client()
    login(c, members['misha']['uid'])
    c.post('/lora-chat/api/send', json={'kind': 'text',
                                        'text': 'секретное слово буратино'})
    M = app.lc_models
    with app.app_context():
        row = M['Message'].query.first()
        assert row.body_enc is not None
        assert b'\xd0\xb1\xd1\x83\xd1\x80\xd0\xb0\xd1\x82' not in row.body_enc
        assert row.body_enc.startswith(b'LC1')


def test_phone_login_returns_token_and_binds_node(app, members):
    c = app.test_client()
    r = c.post('/lora-chat/api/login', json={
        'uid': members['roma']['uid'], 'password': 'test1234',
        'node_id': '0xBBBB0003', 'device_name': 'Pixel', 'can_gateway': True})
    d = r.get_json()
    assert d['ok'] and d['token']
    assert d['member']['name'] == 'Roma'
    # node_id уже был задан хостом — первый вход его не перетирает
    assert d['member']['node_id'] == '0xAAAA0003'


def test_phone_login_wrong_password(app, members):
    c = app.test_client()
    r = c.post('/lora-chat/api/login', json={'uid': members['roma']['uid'],
                                             'password': 'мимо'})
    assert r.status_code == 401


def _phone_token(client, uid, **kw):
    body = {'uid': uid, 'password': 'test1234'}
    body.update(kw)
    return client.post('/lora-chat/api/login', json=body).get_json()['token']


def test_lora_rx_mirrors_air_traffic_into_web(app, members):
    c = app.test_client()
    hdr = {'Authorization': 'Bearer ' + _phone_token(c, members['philip']['uid'])}
    r = c.post('/lora-chat/api/lora_rx', headers=hdr,
               json={'node_id': members['roma']['node'], 'packet_id': 77,
                     'text': 'иду домой'})
    assert r.status_code == 200

    h = c.get('/lora-chat/api/history', headers=hdr).get_json()
    msg = h['messages'][-1]
    assert msg['text'] == 'иду домой'
    assert msg['origin'] == 'lora'
    assert msg['from'] == members['roma']['id'], 'узел должен опознаться как Рома'


def test_lora_rx_deduplicated_across_phones(app, members):
    """Один пакет слышат оба телефона — в чате он обязан быть один.

    Без этого каждое сообщение из эфира двоилось бы по числу
    подключённых шлюзов, а в меше их бывает и пять.
    """
    c1, c2 = app.test_client(), app.test_client()
    h1 = {'Authorization': 'Bearer ' + _phone_token(c1, members['philip']['uid'])}
    h2 = {'Authorization': 'Bearer ' + _phone_token(c2, members['misha']['uid'])}
    packet = {'node_id': members['roma']['node'], 'packet_id': 42, 'text': 'эхо'}

    a = c1.post('/lora-chat/api/lora_rx', headers=h1, json=packet).get_json()
    b = c2.post('/lora-chat/api/lora_rx', headers=h2, json=packet).get_json()
    assert a['msg_id'] == b['msg_id']
    assert b.get('duplicate') is True

    M = app.lc_models
    with app.app_context():
        assert M['Message'].query.filter_by(lora_packet_id=42).count() == 1


def test_lora_rx_from_unknown_node_still_stored(app, members):
    """Чужой узел или датчик — не повод терять пакет: покажем как есть."""
    c = app.test_client()
    hdr = {'Authorization': 'Bearer ' + _phone_token(c, members['philip']['uid'])}
    r = c.post('/lora-chat/api/lora_rx', headers=hdr,
               json={'node_id': '0xDEAD0001', 'packet_id': 5,
                     'text': '18.4C', 'kind': 'sensor', 'name': 'метеостанция'})
    assert r.status_code == 200
    h = c.get('/lora-chat/api/history', headers=hdr).get_json()
    msg = h['messages'][-1]
    assert msg['from'] is None and msg['from_node'] == '0xDEAD0001'
    assert msg['from_name'] == 'метеостанция'


def test_browser_cannot_post_air_traffic(app, members):
    """Вкладка браузера радио не слышит — принимать от неё «эфир» нельзя,
    иначе кто угодно с сессией подделает чужое сообщение из леса."""
    c = app.test_client()
    login(c, members['misha']['uid'])
    r = c.post('/lora-chat/api/lora_rx',
               json={'node_id': members['roma']['node'], 'text': 'подделка'})
    assert r.status_code == 401


def test_media_quota_enforced(app, members):
    import io
    M = app.lc_models
    with app.app_context():
        net = M['Network'].query.first()
        net.quota_bytes = 10          # заведомо меньше любого файла
        from lorachat_dev import db
        db.session.commit()

    c = app.test_client()
    login(c, members['misha']['uid'])
    r = c.post('/lora-chat/api/media',
               data={'file': (io.BytesIO(b'x' * 5000), 'big.png', 'image/png')},
               content_type='multipart/form-data')
    assert r.status_code == 413
    assert 'квота' in r.get_json()['desc']


def test_media_upload_and_fetch(app, members):
    import io
    c = app.test_client()
    login(c, members['misha']['uid'])
    payload = b'\x89PNG' + b'x' * 200
    r = c.post('/lora-chat/api/media',
               data={'file': (io.BytesIO(payload), 'pic.png', 'image/png')},
               content_type='multipart/form-data')
    assert r.status_code == 200
    mid = r.get_json()['media']['id']

    got = c.get('/lora-chat/api/media/%d' % mid)
    assert got.status_code == 200
    assert got.data == payload, 'файл должен расшифровываться обратно байт в байт'


def test_media_stored_encrypted(app, members):
    import io
    c = app.test_client()
    login(c, members['misha']['uid'])
    payload = b'MAGIC-CONTENT-' + b'z' * 100
    r = c.post('/lora-chat/api/media',
               data={'file': (io.BytesIO(payload), 'f.bin', 'application/pdf')},
               content_type='multipart/form-data')
    mid = r.get_json()['media']['id']
    M = app.lc_models
    with app.app_context():
        media = M['Media'].query.get(mid)
        raw = open(media.path, 'rb').read()
    assert b'MAGIC-CONTENT-' not in raw
    assert raw.startswith(b'LC1')


def test_media_type_rejected(app, members):
    import io
    c = app.test_client()
    login(c, members['misha']['uid'])
    r = c.post('/lora-chat/api/media',
               data={'file': (io.BytesIO(b'#!/bin/sh\n'), 'x.sh', 'text/x-sh')},
               content_type='multipart/form-data')
    assert r.status_code == 413


def test_media_of_other_network_not_readable(app, members, tmp_path):
    import io
    M = app.lc_models
    c = app.test_client()
    login(c, members['misha']['uid'])
    r = c.post('/lora-chat/api/media',
               data={'file': (io.BytesIO(b'data' * 50), 'a.png', 'image/png')},
               content_type='multipart/form-data')
    mid = r.get_json()['media']['id']
    with app.app_context():
        from lorachat_dev import db
        M['Media'].query.get(mid).network_id = 999    # как будто чужая сеть
        db.session.commit()
    assert c.get('/lora-chat/api/media/%d' % mid).status_code == 404


def test_host_admin_requires_owner(app):
    c = app.test_client()
    assert c.get('/lora-chat/net/test').status_code in (302, 403)


def test_host_can_add_member_and_sees_password_once(app):
    c = app.test_client()
    c.get('/login')                       # заглушка входа хоста
    r = c.post('/lora-chat/net/test/member',
               data={'username': 'katya', 'display_name': 'Катя'},
               follow_redirects=True)
    assert r.status_code == 200
    body = r.data.decode()
    assert 'katya' in body and 'Данные для' in body
    # Повторное открытие страницы пароль уже не показывает
    again = c.get('/lora-chat/net/test').data.decode()
    assert 'Данные для' not in again


def test_duplicate_username_rejected(app):
    c = app.test_client()
    c.get('/login')
    r = c.post('/lora-chat/net/test/member', data={'username': 'misha'},
               follow_redirects=True)
    assert 'уже занят' in r.data.decode()


def test_password_reset_kills_device_tokens(app, members):
    c = app.test_client()
    token = c.post('/lora-chat/api/login',
                   json={'uid': members['roma']['uid'],
                         'password': 'test1234'}).get_json()['token']
    hdr = {'Authorization': 'Bearer ' + token}
    assert c.get('/lora-chat/api/history', headers=hdr).status_code == 200

    host = app.test_client()
    host.get('/login')
    host.post('/lora-chat/net/test/member/%d/reset' % members['roma']['id'],
              follow_redirects=True)
    assert c.get('/lora-chat/api/history', headers=hdr).status_code == 401


def test_disabled_member_cannot_log_in(app, members):
    host = app.test_client()
    host.get('/login')
    host.post('/lora-chat/net/test/member/%d/toggle' % members['roma']['id'],
              follow_redirects=True)
    c = app.test_client()
    r = c.post('/lora-chat/api/login', json={'uid': members['roma']['uid'],
                                             'password': 'test1234'})
    assert r.status_code == 401
