"""
Шина EasyBridge LoRa Chat: живые соединения и выбор маршрута.

Здесь решается главный вопрос модуля — **как доставить сообщение**.
Маршрутов два, и они не взаимозаменяемы:

  веб  — широкий, быстрый, но требует интернета у ОБОИХ;
  LoRa — узкий (пара сотен байт), медленный, зато работает там, где
         связи нет вообще, и именно ради него всё затевалось.

Правило простое: **веб не заменяет эфир, он его дополняет**. Если
получатель сейчас в вебе — доставляем по интернету, это дешевле и
позволяет отправить фото в нормальном качестве. Если нет — сообщение
обязано уйти в эфир, и его отправляет чей-нибудь телефон, работающий
шлюзом. SOS уходит обоими маршрутами всегда, без экономии.

Модуль не знает про Flask и SQLAlchemy: ему передают уже готовые
объекты и функции. Так его проще тестировать и, если понадобится,
унести в отдельный процесс.
"""

import json
import threading
import time


# ── Одно соединение ─────────────────────────────────────────
class Conn:
    """Обёртка над WebSocket. Знает, кто на том конце.

    Отправка защищена локом: в один сокет могут писать сразу несколько
    потоков (входящее сообщение, presence, команда шлюзу), а
    simple-websocket этого не любит — кадры перемешаются.
    """

    def __init__(self, ws, network_id, member_id, device_id, kind, can_gateway):
        self.ws = ws
        self.network_id = network_id
        self.member_id = member_id
        self.device_id = device_id
        self.kind = kind                  # phone | web | mesh
        self.can_gateway = can_gateway
        self.opened_at = time.time()
        self.last_rx = time.time()
        self.alive = True
        self._lock = threading.Lock()

    def send(self, payload):
        if not self.alive:
            return False
        data = json.dumps(payload, ensure_ascii=False)
        try:
            with self._lock:
                self.ws.send(data)
            return True
        except Exception:
            # Порвалось — не наша забота чинить: пометим мёртвым, хаб
            # выкинет на следующей уборке.
            self.alive = False
            return False

    def send_bytes(self, data):
        """Бинарный кадр (звук). Отдельный метод, а не ветка в send():
        аудио идёт десятками кадров в секунду, и лишняя проверка типа
        на этом пути не нужна.
        """
        if not self.alive:
            return False
        try:
            with self._lock:
                self.ws.send(data)
            return True
        except Exception:
            self.alive = False
            return False

    def close(self):
        self.alive = False
        try:
            self.ws.close()
        except Exception:
            pass


# ── Реестр и маршрутизация ──────────────────────────────────
class Hub:
    def __init__(self, log=None):
        self._lock = threading.RLock()
        # network_id -> device_id -> Conn
        self._nets = {}
        self._log = log or (lambda *a, **k: None)

    # ── реестр ──
    def add(self, conn):
        with self._lock:
            self._nets.setdefault(conn.network_id, {})[conn.device_id] = conn
        self._log('lorachat: подключился device=%s member=%s (%s)',
                  conn.device_id, conn.member_id, conn.kind)

    def remove(self, conn):
        with self._lock:
            net = self._nets.get(conn.network_id)
            if net and net.get(conn.device_id) is conn:
                net.pop(conn.device_id, None)
                if not net:
                    self._nets.pop(conn.network_id, None)
        conn.alive = False

    def conns(self, network_id):
        with self._lock:
            return [c for c in self._nets.get(network_id, {}).values() if c.alive]

    def member_conns(self, network_id, member_id):
        return [c for c in self.conns(network_id) if c.member_id == member_id]

    def online_members(self, network_id):
        return {c.member_id for c in self.conns(network_id)}

    def is_online(self, network_id, member_id):
        return any(c.member_id == member_id for c in self.conns(network_id))

    # ── отправка ──
    def send_to_member(self, network_id, member_id, payload, exclude_device=None):
        sent = 0
        for c in self.member_conns(network_id, member_id):
            if exclude_device and c.device_id == exclude_device:
                continue
            if c.send(payload):
                sent += 1
        return sent

    def broadcast(self, network_id, payload, exclude_device=None):
        sent = 0
        for c in self.conns(network_id):
            if exclude_device and c.device_id == exclude_device:
                continue
            if c.send(payload):
                sent += 1
        return sent

    # ── шлюз в эфир ──
    def pick_gateway(self, network_id, prefer_member_id=None, exclude_device=None):
        """Чей телефон отправит пакет в эфир.

        Приоритет у телефона самого отправителя: он заведомо рядом с
        человеком, его радио — то самое, чей node_id стоит в сообщении,
        и лишнего прыжка по мешу не будет. Если его нет в сети (человек
        пишет с компа, телефон дома) — берём любой другой подключённый
        телефон-шлюз, начиная с того, кто дольше на связи: он с большей
        вероятностью стоит стационарно, а не едет в метро.
        """
        candidates = [c for c in self.conns(network_id)
                      if c.can_gateway and c.kind == 'phone'
                      and c.device_id != exclude_device]
        if not candidates:
            return None
        own = [c for c in candidates if c.member_id == prefer_member_id]
        if own:
            return min(own, key=lambda c: c.opened_at)
        return min(candidates, key=lambda c: c.opened_at)

    # ── решение о маршруте ──
    def plan(self, network_id, recipients, sender_member_id, kind,
             sender_device_id=None):
        """Кому как доставляем.

        recipients — id участников-получателей (для широковещательного
        сообщения это все, кроме отправителя).

        Возвращает (web_ids, lora_ids): первым — кто получит по
        интернету, вторым — для кого нужен эфир. Множества могут
        пересекаться: SOS идёт обоими путями намеренно, чтобы человека
        услышали и за компом, и в лесу.
        """
        web, lora = set(), set()
        for mid in recipients:
            if self.is_online(network_id, mid):
                web.add(mid)
            else:
                # Не в вебе — значит, только эфир. Это и есть случай
                # «Роме пишем из браузера, а он с мештастиком в лесу».
                lora.add(mid)

        if kind == 'sos':
            # Дублируем всем и везде: цена ложного дубля несравнима с
            # ценой недоставленного SOS.
            lora |= set(recipients)
            web |= self.online_members(network_id) - {sender_member_id}

        return web, lora

    def stats(self):
        with self._lock:
            return {
                'networks': len(self._nets),
                'connections': sum(len(v) for v in self._nets.values()),
            }

    def sweep(self):
        """Выкинуть мёртвые соединения. Зовётся из фонового цикла."""
        dropped = []
        with self._lock:
            for net_id, devices in list(self._nets.items()):
                for dev_id, conn in list(devices.items()):
                    if not conn.alive:
                        devices.pop(dev_id, None)
                        dropped.append((net_id, conn.member_id, dev_id))
                if not devices:
                    self._nets.pop(net_id, None)
        return dropped
