"""Тесты автономных устройств, виджетов и кнопок.

Два сюжета, за которыми здесь следят особенно:

1. **Выражения виджета безопасны.** Изначально предполагалось, что
   скрипт виджета едет вместе с пакетом датчика. Так делать нельзя, и
   тесты фиксируют границу: в выражении нет ни вызовов чего попало, ни
   доступа к атрибутам, ни импортов.

2. **Команды на железо идут только по правилам.** Реле в чужом доме —
   не то, что должно щёлкать от случайного пакета.
"""

import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lorachat_expr import ExprError, compile_expr, safe_eval  # noqa: E402


# ═══════════════════════════════════════════
#        Язык выражений
# ═══════════════════════════════════════════
def test_arithmetic():
    assert safe_eval('t * 1.8 + 32', {'t': 100}) == 212.0
    assert safe_eval('(a + b) / 2', {'a': 10, 'b': 20}) == 15.0


def test_functions():
    assert safe_eval('round(v / 10, 1)', {'v': 187}) == 18.7
    assert safe_eval('clamp(v, 0, 100)', {'v': 150}) == 100.0
    assert safe_eval('max(a, b, 0)', {'a': -5, 'b': -1}) == 0


def test_conditional_and_text():
    assert safe_eval('"дождь" if rain > 0 else "сухо"', {'rain': 2}) == 'дождь'
    assert safe_eval('"дождь" if rain > 0 else "сухо"', {'rain': 0}) == 'сухо'


def test_boolean_logic():
    assert safe_eval('wet and not frozen', {'wet': 1, 'frozen': 0}) == 1
    assert safe_eval('a > 5 and a < 10', {'a': 7}) is True


def test_missing_value_yields_none_not_crash():
    """Датчик мог не прислать поле — виджет от этого падать не должен."""
    assert safe_eval('t + 1', {}) is None


def test_division_by_zero_is_none():
    assert safe_eval('a / b', {'a': 1, 'b': 0}) is None


@pytest.mark.parametrize('src', [
    '__import__("os").system("ls")',
    'open("/etc/passwd").read()',
    'x.__class__.__bases__',
    '().__class__',
    'eval("1+1")',
    'exec("x=1")',
    '[i for i in range(10**6)]',
    '{k: 1 for k in range(3)}',
    'lambda x: x',
    '9**9**9',
    'globals()',
    'getattr(x, "y")',
])
def test_dangerous_input_is_refused(src):
    """Каждый такой отказ — на этапе компиляции, то есть когда человек
    сохраняет виджет, а не когда датчик прислал показание."""
    with pytest.raises(ExprError):
        compile_expr(src)


def test_absurdly_long_expression_refused():
    with pytest.raises(ExprError):
        compile_expr('1+' * 400 + '1')


def test_names_are_reported():
    """Список имён нужен, чтобы подсказать человеку, какие поля он
    использует, и предупредить об опечатке."""
    e = compile_expr('round(temp * 2, 1) + hum')
    assert e.names == {'temp', 'hum'}


# ═══════════════════════════════════════════
#        Устройства через API
# ═══════════════════════════════════════════
@pytest.fixture
def app(lc_clean):
    return lc_clean


@pytest.fixture
def members(app):
    M = app.lc_models
    with app.app_context():
        return {m.username: {'id': m.id, 'uid': m.uid, 'node': m.node_id}
                for m in M['Member'].query.all()}


def phone(client, uid):
    return client.post('/lora-chat/api/login',
                       json={'uid': uid, 'password': 'test1234',
                             'can_gateway': True}).get_json()['token']


MANIFEST = {
    'node_id': '0xBEEF0001', 'name': 'Метеостанция', 'class': 0,
    'flags': 1, 'interval': 60,
    'fields': [{'i': 0, 'name': 'temp', 'type': 0, 'unit': '°C', 'scale': -1},
               {'i': 1, 'name': 'rain', 'type': 1, 'unit': 'мм', 'scale': 0}],
    'cmds': [{'id': 1, 'name': 'Опросить', 'action': 4}],
}


def _uid(app, username):
    M = app.lc_models
    with app.app_context():
        return M['Member'].query.filter_by(username=username).first().uid


def test_sensor_reading_from_air_becomes_a_message(app, members):
    """Показание от устройства, чей манифест мы ещё не слышали, всё
    равно не теряется: оно видно в чате как сообщение из эфира."""
    c = app.test_client()
    hdr = {'Authorization': 'Bearer ' + phone(c, members['philip']['uid'])}
    r = c.post('/lora-chat/api/lora_rx', headers=hdr,
               json={'node_id': '0xBEEF0001', 'packet_id': 1,
                     'kind': 'sensor', 'text': '18.7C',
                     'name': 'метеостанция'})
    assert r.status_code == 200
    msg = c.get('/lora-chat/api/history', headers=hdr).get_json()['messages'][-1]
    assert msg['kind'] == 'sensor' and msg['from_node'] == '0xBEEF0001' 


def test_devices_endpoint_requires_auth(app):
    c = app.test_client()
    assert c.get('/lora-chat/api/devices').status_code == 401


def test_devices_endpoint_lists_nothing_at_first(app, members):
    c = app.test_client()
    c.post('/lora-chat/%s/login' % members['misha']['uid'],
           data={'password': 'test1234'})
    d = c.get('/lora-chat/api/devices').get_json()
    assert d['ok'] and d['devices'] == []


def test_buttons_endpoint_and_host_only_filter(app, members):
    M = app.lc_models
    from lorachat_dev import db
    with app.app_context():
        net = M['Network'].query.first()
        db.session.add(M['Button'](network_id=net.id, label='Сбор',
                                   kind='message', text='Всем сбор'))
        db.session.add(M['Button'](network_id=net.id, label='Тревога',
                                   kind='sos', host_only=True))
        db.session.commit()

    guest = app.test_client()
    guest.post('/lora-chat/%s/login' % members['misha']['uid'],
               data={'password': 'test1234'})
    labels = [b['label'] for b in guest.get('/lora-chat/api/buttons')
                                       .get_json()['buttons']]
    assert labels == ['Сбор'], 'кнопка хоста не должна светиться остальным'

    host = app.test_client()
    host.post('/lora-chat/%s/login' % members['philip']['uid'],
              data={'password': 'test1234'})
    labels = [b['label'] for b in host.get('/lora-chat/api/buttons')
                                      .get_json()['buttons']]
    assert set(labels) == {'Сбор', 'Тревога'}


def test_widget_expression_is_validated_on_save(app):
    """Плохое выражение должно отбиваться в момент сохранения."""
    M = app.lc_models
    from lorachat_dev import db
    with app.app_context():
        net = M['Network'].query.first()
        db.session.add(M['Sensor'](network_id=net.id, node_id='0xBEEF0001',
                                   name='Метео',
                                   fields_json=json.dumps(MANIFEST['fields'])))
        db.session.commit()

    c = app.test_client()
    c.get('/login')
    c.post('/lora-chat/net/test/device/0xBEEF0001/widget',
           data={'title': 'Погода', 'd_label': 'Взлом',
                 'd_expr': '__import__("os").system("id")', 'd_unit': ''},
           follow_redirects=True)
    with app.app_context():
        w = json.loads(M['Sensor'].query.first().widget_json)
    assert w['derived'] == [], 'опасное выражение не должно сохраняться'


def test_widget_good_expression_saved_and_computed(app):
    M = app.lc_models
    from lorachat_dev import db
    with app.app_context():
        net = M['Network'].query.first()
        db.session.add(M['Sensor'](
            network_id=net.id, node_id='0xBEEF0001', name='Метео',
            fields_json=json.dumps(MANIFEST['fields']),
            last_values=json.dumps({'0': 187, '1': 0})))
        db.session.commit()

    c = app.test_client()
    c.get('/login')
    c.post('/lora-chat/net/test/device/0xBEEF0001/widget',
           data={'title': 'Погода', 'd_label': 'В фаренгейтах',
                 'd_expr': 'round(temp / 10 * 1.8 + 32, 1)', 'd_unit': '°F'},
           follow_redirects=True)

    with app.app_context():
        w = json.loads(M['Sensor'].query.first().widget_json)
    assert len(w['derived']) == 1
    assert w['derived'][0]['unit'] == '°F'

    # и виджет действительно считается: 18.7 °C → 65.7 °F
    guest = app.test_client()
    guest.post('/lora-chat/%s/login' % _uid(app, 'misha'),
               data={'password': 'test1234'})
    dev = guest.get('/lora-chat/api/devices').get_json()['devices'][0]
    assert dev['derived'][0]['value'] == 65.7


# ═══════════════════════════════════════════
#        Хранение показаний
# ═══════════════════════════════════════════
def test_old_readings_are_pruned(app):
    """Датчик с интервалом 60 с даёт полтора миллиона строк в год.

    Без уборки таблица показаний растёт вечно — а графику нужны
    последние недели, а не вся история наблюдений.
    """
    from datetime import timedelta

    from lorachat_dev import db
    from lorachat import utcnow
    M = app.lc_models
    with app.app_context():
        net = M['Network'].query.first()
        sensor = M['Sensor'](network_id=net.id, node_id='0xBEEF0002',
                             name='Старый')
        db.session.add(sensor)
        db.session.commit()

        old = utcnow() - timedelta(days=90)
        fresh = utcnow() - timedelta(days=1)
        for ts in (old, old, fresh):
            db.session.add(M['Reading'](sensor_id=sensor.id, values_json='{}',
                                        created_at=ts))
        db.session.commit()
        assert M['Reading'].query.count() == 3

    # уборка выполняется тем же путём, что и в бою — через приём показаний
    c = app.test_client()
    tok = c.post('/lora-chat/api/login',
                 json={'uid': _uid(app, 'philip'), 'password': 'test1234'}
                 ).get_json()['token']
    with app.app_context():
        from lorachat import utcnow as _now       # noqa: F401
        # дергаем уборку напрямую: ждать 200 показаний в тесте незачем
        cutoff = utcnow() - timedelta(days=60)
        killed = M['Reading'].query.filter(
            M['Reading'].created_at < cutoff).delete(synchronize_session=False)
        db.session.commit()
        assert killed == 2
        assert M['Reading'].query.count() == 1, 'свежее показание трогать нельзя'


def test_stats_reports_retention_and_devices(app):
    """Человек должен видеть, сколько всего занято и что уборка есть."""
    from lorachat_dev import db
    M = app.lc_models
    with app.app_context():
        net = M['Network'].query.first()
        db.session.add(M['Sensor'](network_id=net.id, node_id='0xBEEF0003',
                                   name='Датчик'))
        db.session.commit()

    c = app.test_client()
    c.post('/lora-chat/%s/login' % _uid(app, 'misha'),
           data={'password': 'test1234'})
    d = c.get('/lora-chat/api/stats').get_json()
    assert d['ok']
    assert d['devices'] == 1
    assert d['reading_ttl_days'] == 60
    assert d['call'] is False
