"""Тесты звонков: арбитраж эфира и склейка двух плеч.

Проверяется главное свойство: **веб-участники слышат друг друга
свободно, а в эфир уходит ровно один голос**. Полудуплексное радио не
прощает двух одновременных говорящих, и ошибка здесь звучит не как
баг, а как «связь плохая».
"""

import os
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from lorachat_call import (AUDIO_HEADER, CODEC_OPUS, CODEC_PCM16,  # noqa: E402
                           FLOOR_HOLD_MS, CallManager, pack_audio, parse_audio)


# ═══════════════════════════════════════════
#            Кадры
# ═══════════════════════════════════════════
def test_audio_frame_roundtrip():
    raw = pack_audio(b'\x01\x02\x03', member_id=258, seq=7, codec=CODEC_OPUS)
    assert len(raw) == AUDIO_HEADER + 3
    got = parse_audio(raw)
    assert got['member'] == 258 and got['seq'] == 7
    assert got['codec'] == CODEC_OPUS and got['payload'] == b'\x01\x02\x03'
    assert got['from_air'] is False


def test_audio_frame_marks_air_origin():
    got = parse_audio(pack_audio(b'x', 0, 1, CODEC_PCM16, from_air=True))
    assert got['from_air'] is True


def test_non_audio_binary_is_ignored():
    assert parse_audio(b'not-an-audio-frame') is None
    assert parse_audio(b'') is None
    assert parse_audio('строка') is None  # не bytes вовсе


def test_large_member_id_survives():
    got = parse_audio(pack_audio(b'', 65535, 65535, CODEC_OPUS))
    assert got['member'] == 65535 and got['seq'] == 65535


# ═══════════════════════════════════════════
#            Звонок и слово
# ═══════════════════════════════════════════
@pytest.fixture
def mgr():
    return CallManager()


def test_start_creates_call(mgr):
    call, created = mgr.start(network_id=1, member_id=10, device_id=100)
    assert created and call.web == {10: call.web[10]}
    assert mgr.for_network(1) is call


def test_second_caller_joins_the_same_call(mgr):
    """Радиоканал один — двух разговоров в эфире всё равно не будет."""
    call, _ = mgr.start(1, 10, 100)
    same, created = mgr.start(1, 11, 110)
    assert same is call and created is False
    assert set(call.web) == {10, 11}


def test_floor_is_exclusive(mgr):
    call, _ = mgr.start(1, 10, 100)
    call.join_web(11, 110, CODEC_OPUS)
    assert call.take_floor(10) is True
    assert call.take_floor(11) is False, 'в эфире может говорить только один'
    assert call.floor == 10


def test_floor_holder_keeps_it_while_speaking(mgr):
    call, _ = mgr.start(1, 10, 100)
    call.take_floor(10)
    for _ in range(5):
        assert call.take_floor(10) is True
    assert call.floor == 10


def test_floor_released_after_silence(mgr):
    """Пауза между фразами не должна отдавать микрофон, а вот конец
    реплики — должен."""
    call, _ = mgr.start(1, 10, 100)
    call.take_floor(10)
    now = time.time()
    assert call.floor_free(now) is False
    assert call.floor_free(now + FLOOR_HOLD_MS / 1000 + 0.1) is True
    assert call.take_floor(11, now + FLOOR_HOLD_MS / 1000 + 0.1) is True
    assert call.floor == 11


def test_leaving_frees_the_floor(mgr):
    """Иначе ушедший со связи запер бы эфир до таймаута."""
    call, _ = mgr.start(1, 10, 100)
    call.join_web(11, 110, CODEC_OPUS)
    call.take_floor(10)
    call.leave_web(10)
    assert call.floor is None
    assert call.take_floor(11) is True


def test_call_dies_when_everyone_left(mgr):
    mgr.start(1, 10, 100)
    call, ended = mgr.leave(1, 10)
    assert ended is True
    assert mgr.for_network(1) is None


def test_call_ends_when_web_side_is_empty(mgr):
    """Ушли все из веба — мост больше не нужен, даже если в эфире
    кто-то есть.

    Связь при этом не рвётся: люди с рациями продолжают говорить друг
    с другом напрямую. Мы лишь перестаём переносить их голос в веб, и
    шлюз освобождает радио под обычный LoRa-трафик.
    """
    call, _ = mgr.start(1, 10, 100)
    call.note_air_node('0xAAA1', 'Рома')
    call.leave_web(10)
    assert call.is_empty() is True


def test_join_unknown_call_returns_none(mgr):
    assert mgr.join(99, 10, 100) is None


def test_sweep_removes_idle_calls(mgr):
    call, _ = mgr.start(1, 10, 100)
    call.leave_web(10)
    assert mgr.sweep() and mgr.for_network(1) is None


def test_sweep_keeps_live_calls(mgr):
    mgr.start(1, 10, 100)
    assert mgr.sweep() == []
    assert mgr.for_network(1) is not None


def test_state_reports_bridging(mgr):
    call, _ = mgr.start(1, 10, 100)
    assert call.state()['bridged'] is False
    call.note_air_node('0xAAA1', 'Рома')
    st = call.state()
    assert st['bridged'] is True
    assert st['air'] == [{'node': '0xAAA1', 'name': 'Рома'}]
