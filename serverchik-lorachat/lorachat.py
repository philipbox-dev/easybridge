"""
EasyBridge LoRa Chat — веб-зеркало LoRa-сети.

Идея: сообщения, голос и фото ходят по LoRa между устройствами, а
сервер держит их веб-копию. Кто сидит за компом — пишет из браузера,
и его сообщение уходит адресату либо по интернету (если тот тоже в
вебе), либо по эфиру через телефон-шлюз. Веб **не заменяет** LoRa: он
второй маршрут, который дешевле и шире, когда интернет есть.

Роли:
  хост      — зарегистрированный юзер сайта, создаёт сеть и участников;
  участник  — логин/пароль + персональная ссылка /lora-chat/<uid>/chat,
              аккаунт на сайте ему не нужен;
  устройство— телефон с EasyLink или вкладка браузера; телефон вдобавок
              работает шлюзом в эфир.

Модуль намеренно самодостаточен: модели, роуты и шина не лезут в
app.py дальше одной строки register_blueprint. Так его можно будет
унести на отдельный сервер, если сеть перерастёт сайт.
"""

import hashlib
import json
import os
import secrets
import string
import time
from datetime import datetime, timedelta, timezone

from flask import (
    Blueprint, abort, current_app, g, jsonify, redirect,
    render_template, request, send_file, session, url_for
)
from flask_login import current_user
from werkzeug.security import check_password_hash, generate_password_hash
from werkzeug.utils import secure_filename

import re

from lorachat_bus import Conn, Hub
from lorachat_expr import ExprError, compile_expr
from lorachat_call import (CODEC_AMR_NB, CODEC_NAMES, CODEC_OPUS, CODEC_PCM16,
                           CallManager, pack_audio, parse_audio)

# ── Настройки ───────────────────────────────────────────────
# Цифры собраны здесь, а не размазаны по коду: их будут крутить.
NET_QUOTA_BYTES = 2 * 1024 ** 3      # 2 ГиБ на сеть
MEDIA_TTL_DAYS = 30                  # текст живёт вечно, файлы — нет
MAX_FILE_BYTES = 50 * 1024 ** 2      # 50 МБ на файл
MAX_TEXT_CHARS = 4000
HISTORY_PAGE = 100

MEDIA_ROOT = os.path.join('static', 'lorachat')

ALLOWED_MIME_PREFIX = ('image/', 'audio/', 'video/')
ALLOWED_MIME_EXTRA = {'application/pdf', 'application/octet-stream'}


def utcnow():
    # naive UTC — как во всей остальной базе сайта
    return datetime.now(timezone.utc).replace(tzinfo=None)


def gen_password(n=10):
    # Без похожих глифов: пароль будут диктовать голосом и вбивать с
    # телефона, и «I» вместо «l» — самая частая жалоба.
    alphabet = ''.join(c for c in string.ascii_letters + string.digits
                       if c not in 'Il1O0oW')
    return ''.join(secrets.choice(alphabet) for _ in range(n))


def gen_uid():
    return secrets.token_urlsafe(9)


def slugify(name):
    keep = [c.lower() if c.isalnum() else '-' for c in name.strip()]
    slug = ''.join(keep).strip('-')
    while '--' in slug:
        slug = slug.replace('--', '-')
    return slug[:40] or 'net'


# ── Шифрование на диске ─────────────────────────────────────
# Сервер ключ знает и при необходимости читает (так решено: нужны
# превью, поиск и восстановление доступа хостом). Смысл шифрования
# здесь другой — унесённый бэкап или украденный диск не должны
# читаться как открытая книга.
class Sealer:
    def __init__(self, master_key):
        self._master = master_key
        try:
            from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        except ImportError as e:      # pragma: no cover
            raise RuntimeError(
                "lorachat: нужен пакет cryptography (pip install cryptography)"
            ) from e
        self._AESGCM = AESGCM

    def _key(self, network_id):
        # Ключ на сеть: компрометация одной не раскрывает соседнюю.
        return hashlib.sha256(
            self._master + b'lc-net-' + str(network_id).encode()).digest()

    def seal(self, network_id, data: bytes) -> bytes:
        if data is None:
            return None
        nonce = os.urandom(12)
        ct = self._AESGCM(self._key(network_id)).encrypt(nonce, data, None)
        return b'LC1' + nonce + ct

    def unseal(self, network_id, blob: bytes) -> bytes:
        if blob is None:
            return None
        if not blob.startswith(b'LC1'):
            # Данные, записанные до включения шифрования, читаем как есть —
            # иначе апгрейд превратил бы всю историю в мусор.
            return blob
        nonce, ct = blob[3:15], blob[15:]
        return self._AESGCM(self._key(network_id)).decrypt(nonce, ct, None)

    def seal_text(self, network_id, text):
        return self.seal(network_id, (text or '').encode('utf-8'))

    def unseal_text(self, network_id, blob):
        if blob is None:
            return ''
        try:
            return self.unseal(network_id, blob).decode('utf-8', 'replace')
        except Exception:
            # Битая или чужая запись не должна ронять весь чат.
            return '[не удалось расшифровать]'


def load_master_key(app):
    """Мастер-ключ в instance/, рядом с secret_key. Один раз генерится."""
    path = os.path.join(app.instance_path, 'lorachat_key')
    try:
        with open(path, 'rb') as f:
            key = f.read().strip()
        if len(key) >= 32:
            return key
    except FileNotFoundError:
        pass
    key = secrets.token_bytes(48)
    os.makedirs(app.instance_path, exist_ok=True)
    with open(path, 'wb') as f:
        f.write(key)
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    return key


def create_lorachat_blueprint(db, csrf=None, sock=None):
    """Собирает блюпринт. sock — экземпляр flask_sock.Sock (может быть None:
    тогда веб-чат работает без реалтайма, на одном REST)."""

    bp = Blueprint('lorachat', __name__, url_prefix='/lora-chat',
                   template_folder='templates')
    if csrf:
        # Устройства ходят с токеном в заголовке, а не с формой и куки —
        # CSRF для них бессмысленен. Веб-формы хоста защищены отдельно
        # (см. _check_csrf ниже).
        csrf.exempt(bp)

    # ═══════════════════════════════════════════
    #                   МОДЕЛИ
    # ═══════════════════════════════════════════

    class LcNetwork(db.Model):
        __tablename__ = 'lc_network'
        __table_args__ = {'extend_existing': True}
        id = db.Column(db.Integer, primary_key=True)
        owner_id = db.Column(db.Integer, index=True, nullable=False)  # User.id сайта
        name = db.Column(db.String(80), nullable=False)
        slug = db.Column(db.String(48), unique=True, nullable=False)
        created_at = db.Column(db.DateTime, default=utcnow)
        quota_bytes = db.Column(db.BigInteger, default=NET_QUOTA_BYTES)
        used_bytes = db.Column(db.BigInteger, default=0)
        media_ttl_days = db.Column(db.Integer, default=MEDIA_TTL_DAYS)
        # SOS дублируется всем и всегда, но выключатель пусть будет:
        # у кого-то сеть на 40 человек и ложные срабатывания.
        sos_broadcast = db.Column(db.Boolean, default=True)
        active = db.Column(db.Boolean, default=True)

    class LcMember(db.Model):
        __tablename__ = 'lc_member'
        __table_args__ = (db.UniqueConstraint('network_id', 'username',
                                              name='uq_lc_member_login'),
                          {'extend_existing': True})
        id = db.Column(db.Integer, primary_key=True)
        network_id = db.Column(db.Integer, index=True, nullable=False)
        uid = db.Column(db.String(24), unique=True, nullable=False)  # для ссылки
        username = db.Column(db.String(40), nullable=False)
        password_hash = db.Column(db.String(256))
        display_name = db.Column(db.String(60))
        # node_id устройства в эфире, '0x1A2B3C4D'. Пока участник не
        # подключил телефон, он известен только вебу.
        node_id = db.Column(db.String(16), index=True)
        is_host = db.Column(db.Boolean, default=False)
        active = db.Column(db.Boolean, default=True)
        created_at = db.Column(db.DateTime, default=utcnow)
        last_seen_at = db.Column(db.DateTime)

        @property
        def title(self):
            return self.display_name or self.username

    class LcDevice(db.Model):
        __tablename__ = 'lc_device'
        __table_args__ = {'extend_existing': True}
        id = db.Column(db.Integer, primary_key=True)
        member_id = db.Column(db.Integer, index=True, nullable=False)
        network_id = db.Column(db.Integer, index=True, nullable=False)
        kind = db.Column(db.String(8), default='web')   # phone | web | mesh
        token = db.Column(db.String(64), unique=True, nullable=False)
        name = db.Column(db.String(60))
        hw_profile = db.Column(db.String(32))    # профиль hwcfg, если телефон сказал
        has_fsk = db.Column(db.Boolean, default=False)
        # Телефон, подключённый к устройству, может работать шлюзом в эфир.
        can_gateway = db.Column(db.Boolean, default=False)
        online = db.Column(db.Boolean, default=False)
        last_seen_at = db.Column(db.DateTime)
        created_at = db.Column(db.DateTime, default=utcnow)

    class LcMessage(db.Model):
        __tablename__ = 'lc_message'
        __table_args__ = {'extend_existing': True}
        id = db.Column(db.Integer, primary_key=True)
        network_id = db.Column(db.Integer, index=True, nullable=False)
        # Отправитель может быть узлом, которого нет среди участников
        # (чужой ретранслятор, датчик) — тогда member_id пуст.
        sender_member_id = db.Column(db.Integer, index=True)
        sender_node_id = db.Column(db.String(16))
        sender_name = db.Column(db.String(60))
        target_kind = db.Column(db.String(8), default='all')  # all|member|group
        target_member_id = db.Column(db.Integer, index=True)
        target_group = db.Column(db.String(40))
        kind = db.Column(db.String(10), default='text')
        # text|voice|image|file|sos|sys|sensor
        body_enc = db.Column(db.LargeBinary)     # зашифрованный текст
        media_id = db.Column(db.Integer, index=True)
        origin = db.Column(db.String(6), default='web')   # web|lora|phone
        # Ключ против двойников: одно сообщение приходит и из веба, и
        # эхом из эфира через несколько телефонов сразу.
        dedup_key = db.Column(db.String(64), unique=True)
        lora_packet_id = db.Column(db.Integer)
        created_at = db.Column(db.DateTime, default=utcnow, index=True)

    class LcDelivery(db.Model):
        __tablename__ = 'lc_delivery'
        __table_args__ = (db.UniqueConstraint('message_id', 'member_id',
                                              name='uq_lc_delivery'),
                          {'extend_existing': True})
        id = db.Column(db.Integer, primary_key=True)
        message_id = db.Column(db.Integer, index=True, nullable=False)
        member_id = db.Column(db.Integer, index=True, nullable=False)
        via = db.Column(db.String(4))            # web | lora
        state = db.Column(db.String(10), default='queued')
        # queued | sent | delivered | read | failed
        updated_at = db.Column(db.DateTime, default=utcnow)

    class LcMedia(db.Model):
        __tablename__ = 'lc_media'
        __table_args__ = {'extend_existing': True}
        id = db.Column(db.Integer, primary_key=True)
        network_id = db.Column(db.Integer, index=True, nullable=False)
        uploader_id = db.Column(db.Integer)
        filename = db.Column(db.String(160))
        mime = db.Column(db.String(60))
        size = db.Column(db.Integer, default=0)
        sha256 = db.Column(db.String(64), index=True)
        path = db.Column(db.String(300))
        created_at = db.Column(db.DateTime, default=utcnow)
        expires_at = db.Column(db.DateTime, index=True)

    class LcSensor(db.Model):
        """Автономное устройство: датчик, реле, метеостанция.

        Живёт само по себе, телефона у него нет. В базу попадает из
        манифеста, который оно широковещательно рассказывает о себе,
        — поэтому заводить его руками не нужно: включил и он появился.
        """
        __tablename__ = 'lc_sensor'
        __table_args__ = (db.UniqueConstraint('network_id', 'node_id',
                                              name='uq_lc_sensor_node'),
                          {'extend_existing': True})
        id = db.Column(db.Integer, primary_key=True)
        network_id = db.Column(db.Integer, index=True, nullable=False)
        node_id = db.Column(db.String(16), nullable=False)
        name = db.Column(db.String(60))
        dev_class = db.Column(db.Integer, default=0)
        flags = db.Column(db.Integer, default=0)
        interval_s = db.Column(db.Integer, default=60)
        fields_json = db.Column(db.Text, default='[]')   # схема из манифеста
        cmds_json = db.Column(db.Text, default='[]')
        # Настройка вида виджета: подписи, пороги, производные значения.
        # Задаётся хостом в вебе и НЕ приходит из эфира (см. lorachat_expr).
        widget_json = db.Column(db.Text, default='{}')
        last_seen_at = db.Column(db.DateTime, index=True)
        last_rssi = db.Column(db.Integer)
        last_values = db.Column(db.Text, default='{}')
        created_at = db.Column(db.DateTime, default=utcnow)

        def fields(self):
            try:
                return json.loads(self.fields_json or '[]')
            except ValueError:
                return []

        def cmds(self):
            try:
                return json.loads(self.cmds_json or '[]')
            except ValueError:
                return []

        def widget(self):
            try:
                return json.loads(self.widget_json or '{}')
            except ValueError:
                return {}

        def values(self):
            try:
                return json.loads(self.last_values or '{}')
            except ValueError:
                return {}

    class LcReading(db.Model):
        """История показаний. Чистится по сроку, как и медиа: график за
        месяц полезен, за три года — это просто занятый диск."""
        __tablename__ = 'lc_reading'
        __table_args__ = {'extend_existing': True}
        id = db.Column(db.Integer, primary_key=True)
        sensor_id = db.Column(db.Integer, index=True, nullable=False)
        created_at = db.Column(db.DateTime, default=utcnow, index=True)
        values_json = db.Column(db.Text, default='{}')
        rssi = db.Column(db.Integer)

    class LcButton(db.Model):
        """Кнопка-плагин сети: свои алерты и команды одним нажатием.

        Хост собирает их под свою группу — «сбор», «тревога», «включить
        свет в гараже». Кнопка не несёт кода: только тип действия и
        параметры, проверяемые сервером.
        """
        __tablename__ = 'lc_button'
        __table_args__ = {'extend_existing': True}
        id = db.Column(db.Integer, primary_key=True)
        network_id = db.Column(db.Integer, index=True, nullable=False)
        label = db.Column(db.String(40), nullable=False)
        icon = db.Column(db.String(8), default='')
        kind = db.Column(db.String(10), default='message')  # message|sos|devcmd
        text = db.Column(db.Text, default='')
        node_id = db.Column(db.String(16))     # для devcmd
        cmd_id = db.Column(db.Integer, default=0)
        cmd_arg = db.Column(db.Integer, default=0)
        host_only = db.Column(db.Boolean, default=False)
        confirm = db.Column(db.Boolean, default=False)
        sort = db.Column(db.Integer, default=0)
        created_at = db.Column(db.DateTime, default=utcnow)

    class LcAudit(db.Model):
        __tablename__ = 'lc_audit'
        __table_args__ = {'extend_existing': True}
        id = db.Column(db.Integer, primary_key=True)
        network_id = db.Column(db.Integer, index=True)
        actor = db.Column(db.String(60))
        action = db.Column(db.String(40))
        detail = db.Column(db.Text)
        created_at = db.Column(db.DateTime, default=utcnow)

    models = dict(Network=LcNetwork, Member=LcMember, Device=LcDevice,
                  Message=LcMessage, Delivery=LcDelivery, Media=LcMedia,
                  Audit=LcAudit, Sensor=LcSensor, Reading=LcReading,
                  Button=LcButton)

    # Шифровальщик создаётся лениво: на момент сборки блюпринта
    # приложения ещё нет, а instance_path нужен именно от него.
    state = {'sealer': None}
    hub = Hub()
    calls = CallManager()

    def sealer():
        if state['sealer'] is None:
            app = current_app._get_current_object()
            state['sealer'] = Sealer(load_master_key(app))
            hub._log = app.logger.info
        return state['sealer']

    # ═══════════════════════════════════════════
    #                 ХЕЛПЕРЫ
    # ═══════════════════════════════════════════

    def _err(code, msg, http=400):
        return jsonify({'ok': False, 'error': code, 'desc': msg}), http

    def _check_csrf():
        """Блюпринт исключён из глобальной защиты ради устройств, но
        формы хоста ходят с куки — им CSRF нужен."""
        if not csrf:
            return True
        from flask_wtf.csrf import validate_csrf
        token = (request.form.get('csrf_token') or
                 request.headers.get('X-CSRFToken'))
        try:
            validate_csrf(token)
            return True
        except Exception:
            return False

    def _net_by_slug(slug):
        net = LcNetwork.query.filter_by(slug=slug, active=True).first()
        if not net:
            abort(404)
        return net

    def _require_host(net):
        if not current_user.is_authenticated or current_user.id != net.owner_id:
            abort(403)

    def _audit(net_id, action, detail=''):
        actor = (current_user.username
                 if current_user.is_authenticated
                 else 'аноним')
        db.session.add(LcAudit(network_id=net_id, actor=actor,
                               action=action, detail=detail))

    # ── Кто сейчас в браузере ──
    def _session_member():
        mid = session.get('lc_member_id')
        if not mid:
            return None
        m = db.session.get(LcMember, mid)
        if not m or not m.active:
            session.pop('lc_member_id', None)
            return None
        return m

    def _device_by_token(token):
        if not token:
            return None
        return LcDevice.query.filter_by(token=token).first()

    def _bearer():
        auth = request.headers.get('Authorization', '')
        if auth.startswith('Bearer '):
            return auth[7:].strip()
        return request.args.get('token') or request.form.get('token')

    def _api_device():
        """Устройство по токену, либо виртуальное устройство браузера.

        Браузеру отдельный токен не нужен: он уже опознан сессионной
        кукой, и заводить ему второй секрет — лишняя сущность, которую
        придётся где-то хранить и отзывать.
        """
        dev = _device_by_token(_bearer())
        if dev:
            return dev
        m = _session_member()
        if not m:
            return None
        return _ensure_web_device(m)

    def _ensure_web_device(member):
        key = 'web:%d' % member.id
        dev = LcDevice.query.filter_by(member_id=member.id, kind='web').first()
        if not dev:
            dev = LcDevice(member_id=member.id, network_id=member.network_id,
                           kind='web', token=secrets.token_urlsafe(32),
                           name='Браузер')
            db.session.add(dev)
            db.session.commit()
        return dev

    # ── Сообщения ──
    def _msg_json(msg, media=None):
        out = {
            'id': msg.id,
            'net': msg.network_id,
            'from': msg.sender_member_id,
            'from_name': msg.sender_name,
            'from_node': msg.sender_node_id,
            'target': msg.target_kind,
            'target_id': msg.target_member_id,
            'group': msg.target_group,
            'kind': msg.kind,
            'text': sealer().unseal_text(msg.network_id, msg.body_enc),
            'origin': msg.origin,
            'ts': msg.created_at.isoformat() + 'Z',
        }
        if msg.media_id:
            out['media'] = {'id': msg.media_id,
                            'url': url_for('lorachat.media_get', mid=msg.media_id)}
            if media:
                out['media'].update({'mime': media.mime, 'size': media.size,
                                     'name': media.filename})
        return out

    def _recipients(net, msg, sender_member_id):
        q = LcMember.query.filter_by(network_id=net.id, active=True)
        if msg.target_kind == 'member' and msg.target_member_id:
            return [msg.target_member_id]
        return [m.id for m in q.all() if m.id != sender_member_id]

    def _deliver(net, msg, sender_member_id, sender_device_id=None):
        """Разложить сообщение по маршрутам и разослать.

        Возвращает словарь с тем, что реально произошло — приложению и
        вебу важно показать «ушло по интернету» или «передаётся по
        эфиру», это разные ожидания по времени.
        """
        media = db.session.get(LcMedia, msg.media_id) if msg.media_id else None
        payload = {'op': 'msg', 'msg': _msg_json(msg, media)}
        recipients = _recipients(net, msg, sender_member_id)
        web_ids, lora_ids = hub.plan(net.id, recipients, sender_member_id,
                                     msg.kind, sender_device_id)

        for mid in web_ids:
            hub.send_to_member(net.id, mid, payload)
            db.session.add(LcDelivery(message_id=msg.id, member_id=mid,
                                      via='web', state='sent',
                                      updated_at=utcnow()))

        gateway = None
        if lora_ids:
            gateway = hub.pick_gateway(net.id, prefer_member_id=sender_member_id,
                                       exclude_device=None)
            for mid in lora_ids:
                db.session.add(LcDelivery(
                    message_id=msg.id, member_id=mid, via='lora',
                    state='queued' if gateway else 'failed',
                    updated_at=utcnow()))

        if gateway:
            # Телефон-шлюз получает команду отправить это в эфир.
            # Пометка WEB нужна получателю, у которого телефона под
            # рукой нет: на мештастике он увидит «[WEB] Philip: …» и
            # поймёт, откуда пришло и почему коротко.
            targets = [db.session.get(LcMember, mid) for mid in lora_ids]
            gateway.send({
                'op': 'lora_tx',
                'msg_id': msg.id,
                'kind': msg.kind,
                'text': sealer().unseal_text(msg.network_id, msg.body_enc),
                'from_name': msg.sender_name,
                'web_flag': True,
                'targets': [{'id': t.id, 'node': t.node_id, 'name': t.title}
                            for t in targets if t],
                'has_media': bool(msg.media_id),
                'media_url': (url_for('lorachat.media_get', mid=msg.media_id,
                                      _external=True)
                              if msg.media_id else None),
            })

        db.session.commit()
        return {
            'web': sorted(web_ids),
            'lora': sorted(lora_ids),
            'gateway': gateway.device_id if gateway else None,
            'lora_blocked': bool(lora_ids) and gateway is None,
        }

    def _store_message(net, *, sender_member=None, sender_node=None,
                       sender_name=None, kind='text', text='', media_id=None,
                       target_kind='all', target_member_id=None,
                       target_group=None, origin='web', dedup_key=None,
                       lora_packet_id=None):
        if dedup_key:
            existing = LcMessage.query.filter_by(dedup_key=dedup_key).first()
            if existing:
                # Эхо того же пакета от второго телефона — не дубль.
                return existing, False
        msg = LcMessage(
            network_id=net.id,
            sender_member_id=sender_member.id if sender_member else None,
            sender_node_id=sender_node,
            sender_name=sender_name or (sender_member.title if sender_member
                                        else (sender_node or 'узел')),
            target_kind=target_kind, target_member_id=target_member_id,
            target_group=target_group, kind=kind,
            body_enc=sealer().seal_text(net.id, text[:MAX_TEXT_CHARS]),
            media_id=media_id, origin=origin, dedup_key=dedup_key,
            lora_packet_id=lora_packet_id, created_at=utcnow())
        db.session.add(msg)
        db.session.commit()
        return msg, True

    # ── Медиа ──
    def _media_dir(net):
        d = os.path.join(current_app.root_path, MEDIA_ROOT, net.slug)
        os.makedirs(d, exist_ok=True)
        return d

    def _sweep_expired(net):
        """Убрать протухшие файлы и вернуть освободившееся место.

        Текст остаётся навсегда — он дешёвый. Место ест медиа, поэтому
        чистится оно, а сообщение с истёкшим файлом просто теряет
        вложение и остаётся в истории как «файл удалён по сроку».
        """
        now = utcnow()
        freed = 0
        stale = LcMedia.query.filter(LcMedia.network_id == net.id,
                                     LcMedia.expires_at < now).all()
        for m in stale:
            try:
                if m.path and os.path.exists(m.path):
                    os.remove(m.path)
            except OSError:
                current_app.logger.warning('lorachat: не удалить %s', m.path)
            freed += m.size or 0
            db.session.delete(m)
        if stale:
            net.used_bytes = max(0, (net.used_bytes or 0) - freed)
            db.session.commit()
        return freed

    def _save_media(net, member, fs):
        _sweep_expired(net)

        blob = fs.read()
        if not blob:
            return None, 'пустой файл'
        if len(blob) > MAX_FILE_BYTES:
            return None, 'файл больше %d МБ' % (MAX_FILE_BYTES // 1024 ** 2)

        mime = (fs.mimetype or 'application/octet-stream').lower()
        if not (mime.startswith(ALLOWED_MIME_PREFIX) or mime in ALLOWED_MIME_EXTRA):
            return None, 'тип %s не принимаем' % mime

        if (net.used_bytes or 0) + len(blob) > (net.quota_bytes or NET_QUOTA_BYTES):
            return None, ('квота сети исчерпана (%d МБ) — попросите хоста '
                          'увеличить или подождите, пока протухнут старые файлы'
                          % ((net.quota_bytes or 0) // 1024 ** 2))

        digest = hashlib.sha256(blob).hexdigest()
        dup = LcMedia.query.filter_by(network_id=net.id, sha256=digest).first()
        if dup and dup.path and os.path.exists(dup.path):
            # Тот же файл уже лежит: пересланное фото не должно
            # съедать квоту второй раз.
            dup.expires_at = max(dup.expires_at or utcnow(),
                                 utcnow() + timedelta(days=net.media_ttl_days
                                                      or MEDIA_TTL_DAYS))
            db.session.commit()
            return dup, None

        name = secure_filename(fs.filename or 'file')[:120] or 'file'
        path = os.path.join(_media_dir(net), '%s_%s.bin' % (digest[:16],
                                                            utcnow().strftime('%Y%m')))
        with open(path, 'wb') as f:
            f.write(sealer().seal(net.id, blob))

        media = LcMedia(network_id=net.id, uploader_id=member.id if member else None,
                        filename=name, mime=mime, size=len(blob), sha256=digest,
                        path=path, created_at=utcnow(),
                        expires_at=utcnow() + timedelta(
                            days=net.media_ttl_days or MEDIA_TTL_DAYS))
        db.session.add(media)
        net.used_bytes = (net.used_bytes or 0) + len(blob)
        db.session.commit()
        return media, None

    # ═══════════════════════════════════════════
    #              АДМИНКА ХОСТА
    # ═══════════════════════════════════════════

    @bp.route('/')
    def index():
        if not current_user.is_authenticated:
            return redirect(url_for('login', next=request.path))
        nets = LcNetwork.query.filter_by(owner_id=current_user.id, active=True) \
                              .order_by(LcNetwork.created_at.desc()).all()
        rows = []
        for n in nets:
            rows.append({
                'net': n,
                'members': LcMember.query.filter_by(network_id=n.id,
                                                    active=True).count(),
                'online': len(hub.online_members(n.id)),
            })
        return render_template('lorachat_index.html', rows=rows)

    @bp.route('/create', methods=['POST'])
    def create_net():
        if not current_user.is_authenticated:
            abort(403)
        if not _check_csrf():
            abort(400)
        name = (request.form.get('name') or '').strip()[:80]
        if not name:
            return redirect(url_for('lorachat.index'))

        slug = slugify(name)
        # Слаг уникален глобально: он в URL, и две «Дачи» разных хостов
        # столкнутся. Добавляем суффикс, а не отказываем.
        base, n = slug, 1
        while LcNetwork.query.filter_by(slug=slug).first():
            n += 1
            slug = '%s-%d' % (base, n)

        net = LcNetwork(owner_id=current_user.id, name=name, slug=slug,
                        quota_bytes=NET_QUOTA_BYTES, media_ttl_days=MEDIA_TTL_DAYS)
        db.session.add(net)
        db.session.commit()

        # Хост сразу участник собственной сети: иначе он не сможет ни
        # писать, ни принимать SOS.
        host = LcMember(network_id=net.id, uid=gen_uid(),
                        username=(current_user.username or 'host')[:40],
                        display_name=current_user.username, is_host=True)
        host.password_hash = generate_password_hash(gen_password(12))
        db.session.add(host)
        _audit(net.id, 'net_create', name)
        db.session.commit()
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>')
    def net_admin(slug):
        net = _net_by_slug(slug)
        _require_host(net)
        members = LcMember.query.filter_by(network_id=net.id) \
                                .order_by(LcMember.created_at).all()
        online = hub.online_members(net.id)
        fresh = session.pop('lc_fresh_creds', None)
        sensors = LcSensor.query.filter_by(network_id=net.id) \
                                .order_by(LcSensor.name).all()
        buttons = LcButton.query.filter_by(network_id=net.id) \
                                .order_by(LcButton.sort, LcButton.id).all()
        return render_template('lorachat_admin.html', net=net, members=members,
                               online=online, fresh=fresh,
                               devices=[_sensor_json(d) for d in sensors],
                               buttons=buttons,
                               quota_mb=(net.quota_bytes or 0) // 1024 ** 2,
                               used_mb=(net.used_bytes or 0) // 1024 ** 2)

    @bp.route('/net/<slug>/member', methods=['POST'])
    def member_add(slug):
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)

        username = (request.form.get('username') or '').strip().lower()[:40]
        display = (request.form.get('display_name') or '').strip()[:60]
        node_id = (request.form.get('node_id') or '').strip()[:16] or None
        if not username:
            return redirect(url_for('lorachat.net_admin', slug=slug))
        if LcMember.query.filter_by(network_id=net.id, username=username).first():
            session['lc_fresh_creds'] = {'error': 'Логин %s уже занят' % username}
            return redirect(url_for('lorachat.net_admin', slug=slug))

        password = gen_password()
        m = LcMember(network_id=net.id, uid=gen_uid(), username=username,
                     display_name=display or username, node_id=node_id,
                     password_hash=generate_password_hash(password))
        db.session.add(m)
        _audit(net.id, 'member_add', username)
        db.session.commit()

        # Пароль показывается ровно один раз — дальше только сброс.
        # Хранить его в открытом виде, чтобы «посмотреть потом», значит
        # однажды отдать всю сеть вместе с бэкапом базы.
        session['lc_fresh_creds'] = {
            'username': username, 'password': password,
            'link': url_for('lorachat.chat', uid=m.uid, _external=True),
        }
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>/member/<int:mid>/reset', methods=['POST'])
    def member_reset(slug, mid):
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)
        m = db.session.get(LcMember, mid)
        if not m or m.network_id != net.id:
            abort(404)
        password = gen_password()
        m.password_hash = generate_password_hash(password)
        # Старые сессии и токены устройств после смены пароля недействительны.
        LcDevice.query.filter_by(member_id=m.id).delete()
        _audit(net.id, 'member_reset', m.username)
        db.session.commit()
        session['lc_fresh_creds'] = {
            'username': m.username, 'password': password,
            'link': url_for('lorachat.chat', uid=m.uid, _external=True),
        }
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>/member/<int:mid>/toggle', methods=['POST'])
    def member_toggle(slug, mid):
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)
        m = db.session.get(LcMember, mid)
        if not m or m.network_id != net.id or m.is_host:
            abort(404)
        m.active = not m.active
        if not m.active:
            LcDevice.query.filter_by(member_id=m.id).delete()
            for c in hub.member_conns(net.id, m.id):
                c.close()
        _audit(net.id, 'member_toggle', '%s → %s' % (m.username, m.active))
        db.session.commit()
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>/button', methods=['POST'])
    def button_add(slug):
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)
        label = (request.form.get('label') or '').strip()[:40]
        kind = (request.form.get('kind') or 'message').strip()
        if not label or kind not in ('message', 'sos', 'devcmd'):
            return redirect(url_for('lorachat.net_admin', slug=slug))

        btn = LcButton(network_id=net.id, label=label, kind=kind,
                       icon=(request.form.get('icon') or '')[:8],
                       text=(request.form.get('text') or '')[:1000],
                       node_id=(request.form.get('node_id') or '')[:16] or None,
                       host_only=bool(request.form.get('host_only')),
                       confirm=bool(request.form.get('confirm')) or kind == 'sos',
                       sort=len(LcButton.query.filter_by(network_id=net.id).all()))
        try:
            btn.cmd_id = int(request.form.get('cmd_id') or 0)
            btn.cmd_arg = int(request.form.get('cmd_arg') or 0)
        except ValueError:
            btn.cmd_id = btn.cmd_arg = 0
        db.session.add(btn)
        _audit(net.id, 'button_add', label)
        db.session.commit()
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>/button/<int:bid>/delete', methods=['POST'])
    def button_delete(slug, bid):
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)
        btn = db.session.get(LcButton, bid)
        if btn and btn.network_id == net.id:
            _audit(net.id, 'button_delete', btn.label)
            db.session.delete(btn)
            db.session.commit()
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>/device/<node>/widget', methods=['POST'])
    def device_widget(slug, node):
        """Настройка виджета: подписи и производные значения.

        Выражения проверяются здесь, в момент сохранения. Человек
        увидит ошибку сразу, а не через сутки в виде пустого виджета —
        и, что важнее, заведомо сломанное выражение не попадёт в
        горячий путь показаний.
        """
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)
        sensor = _sensor_by_node(net.id, node)
        if not sensor:
            abort(404)

        derived, errors = [], []
        labels = request.form.getlist('d_label')
        exprs = request.form.getlist('d_expr')
        units = request.form.getlist('d_unit')
        for i, expr_src in enumerate(exprs[:12]):
            expr_src = (expr_src or '').strip()
            if not expr_src:
                continue
            try:
                compile_expr(expr_src)
            except ExprError as e:
                errors.append('%s: %s' % (labels[i] if i < len(labels)
                                          else expr_src, e))
                continue
            derived.append({'label': (labels[i] if i < len(labels) else '')[:40],
                            'expr': expr_src,
                            'unit': (units[i] if i < len(units) else '')[:12]})

        widget = sensor.widget()
        widget['derived'] = derived
        widget['title'] = (request.form.get('title') or '')[:40]
        sensor.widget_json = json.dumps(widget, ensure_ascii=False)
        # Кеш скомпилированных выражений держит и старые: после правки
        # он бы отдавал прошлую версию.
        _expr_cache.clear()
        _audit(net.id, 'widget_edit', node)
        db.session.commit()
        if errors:
            session['lc_fresh_creds'] = {'error': '; '.join(errors)}
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>/device/<node>/forget', methods=['POST'])
    def device_forget(slug, node):
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)
        sensor = _sensor_by_node(net.id, node)
        if sensor:
            LcReading.query.filter_by(sensor_id=sensor.id).delete()
            db.session.delete(sensor)
            _audit(net.id, 'device_forget', node)
            db.session.commit()
        return redirect(url_for('lorachat.net_admin', slug=slug))

    @bp.route('/net/<slug>/settings', methods=['POST'])
    def net_settings(slug):
        net = _net_by_slug(slug)
        _require_host(net)
        if not _check_csrf():
            abort(400)
        try:
            quota_mb = int(request.form.get('quota_mb') or 0)
            ttl = int(request.form.get('ttl_days') or 0)
        except ValueError:
            return redirect(url_for('lorachat.net_admin', slug=slug))
        if quota_mb > 0:
            net.quota_bytes = quota_mb * 1024 ** 2
        if ttl > 0:
            net.media_ttl_days = ttl
        net.sos_broadcast = bool(request.form.get('sos_broadcast'))
        _audit(net.id, 'net_settings', 'quota=%sMB ttl=%s' % (quota_mb, ttl))
        db.session.commit()
        return redirect(url_for('lorachat.net_admin', slug=slug))

    # ═══════════════════════════════════════════
    #            ВЕБ-ЧАТ УЧАСТНИКА
    # ═══════════════════════════════════════════

    @bp.route('/<uid>/chat')
    def chat(uid):
        m = LcMember.query.filter_by(uid=uid, active=True).first()
        if not m:
            abort(404)
        net = db.session.get(LcNetwork, m.network_id)
        if not net or not net.active:
            abort(404)

        cur = _session_member()
        if not cur or cur.id != m.id:
            # Ссылка сама по себе не пропуск: она попадёт в историю
            # браузера, в мессенджер, в скриншот. Пароль обязателен.
            return render_template('lorachat_login.html', member=m, net=net)

        peers = LcMember.query.filter_by(network_id=net.id, active=True).all()
        return render_template(
            'lorachat_room.html', net=net, me=m,
            peers=[p for p in peers if p.id != m.id],
            online=list(hub.online_members(net.id)),
            ws_url=url_for('lorachat.ws_endpoint'),
        )

    @bp.route('/<uid>/login', methods=['POST'])
    def member_login(uid):
        m = LcMember.query.filter_by(uid=uid, active=True).first()
        if not m:
            abort(404)
        password = request.form.get('password') or ''
        if not m.password_hash or not check_password_hash(m.password_hash, password):
            net = db.session.get(LcNetwork, m.network_id)
            return render_template('lorachat_login.html', member=m, net=net,
                                   error='Неверный пароль'), 401
        session['lc_member_id'] = m.id
        session.permanent = True
        m.last_seen_at = utcnow()
        db.session.commit()
        return redirect(url_for('lorachat.chat', uid=uid))

    @bp.route('/<uid>/logout', methods=['POST', 'GET'])
    def member_logout(uid):
        session.pop('lc_member_id', None)
        return redirect(url_for('lorachat.chat', uid=uid))

    # ═══════════════════════════════════════════
    #              API УСТРОЙСТВ
    # ═══════════════════════════════════════════

    @bp.route('/api/login', methods=['POST'])
    def api_login():
        """Логин телефона. Отдаёт токен устройства — он и есть пропуск
        в WebSocket, отдельной сессии приложению не нужно."""
        data = request.get_json(silent=True) or request.form
        username = (data.get('username') or '').strip().lower()
        password = data.get('password') or ''
        net_slug = (data.get('network') or '').strip()
        uid = (data.get('uid') or '').strip()

        m = None
        if uid:
            m = LcMember.query.filter_by(uid=uid, active=True).first()
        elif net_slug:
            net = LcNetwork.query.filter_by(slug=net_slug, active=True).first()
            if net:
                m = LcMember.query.filter_by(network_id=net.id,
                                             username=username, active=True).first()
        if not m or not m.password_hash or \
                not check_password_hash(m.password_hash, password):
            return _err('auth', 'Неверный логин или пароль', 401)

        node_id = (data.get('node_id') or '').strip()[:16] or None
        if node_id and not m.node_id:
            # Первый вход с телефона связывает участника с его узлом в
            # эфире — дальше по node_id мы узнаём его сообщения из LoRa.
            m.node_id = node_id

        dev = LcDevice(
            member_id=m.id, network_id=m.network_id, kind='phone',
            token=secrets.token_urlsafe(32),
            name=(data.get('device_name') or 'Телефон')[:60],
            hw_profile=(data.get('hw_profile') or '')[:32] or None,
            has_fsk=bool(data.get('has_fsk')),
            can_gateway=bool(data.get('can_gateway', True)),
            created_at=utcnow())
        db.session.add(dev)
        m.last_seen_at = utcnow()
        db.session.commit()

        net = db.session.get(LcNetwork, m.network_id)
        return jsonify({
            'ok': True,
            'token': dev.token,
            'member': {'id': m.id, 'uid': m.uid, 'name': m.title,
                       'node_id': m.node_id, 'is_host': m.is_host},
            'network': {'id': net.id, 'slug': net.slug, 'name': net.name},
            'ws': url_for('lorachat.ws_endpoint'),
            'peers': [{'id': p.id, 'name': p.title, 'node_id': p.node_id}
                      for p in LcMember.query.filter_by(network_id=net.id,
                                                        active=True).all()
                      if p.id != m.id],
        })

    @bp.route('/api/logout', methods=['POST'])
    def api_logout():
        dev = _device_by_token(_bearer())
        if dev:
            for c in hub.conns(dev.network_id):
                if c.device_id == dev.id:
                    c.close()
            db.session.delete(dev)
            db.session.commit()
        return jsonify({'ok': True})

    @bp.route('/api/history')
    def api_history():
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        try:
            since = int(request.args.get('since') or 0)
            limit = min(int(request.args.get('limit') or HISTORY_PAGE), 500)
        except ValueError:
            return _err('bad_args', 'since и limit — числа')

        q = LcMessage.query.filter(LcMessage.network_id == dev.network_id)
        if since:
            q = q.filter(LcMessage.id > since)
        rows = q.order_by(LcMessage.id.desc()).limit(limit).all()
        rows.reverse()
        media_ids = [r.media_id for r in rows if r.media_id]
        media = {m.id: m for m in LcMedia.query.filter(
            LcMedia.id.in_(media_ids)).all()} if media_ids else {}
        return jsonify({'ok': True,
                        'messages': [_msg_json(r, media.get(r.media_id))
                                     for r in rows]})

    @bp.route('/api/media', methods=['POST'])
    def media_upload():
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        net = db.session.get(LcNetwork, dev.network_id)
        member = db.session.get(LcMember, dev.member_id)
        fs = request.files.get('file')
        if not fs:
            return _err('no_file', 'Нет файла в запросе')

        media, err = _save_media(net, member, fs)
        if err:
            return _err('media', err, 413)

        # Файл сам по себе — ещё не сообщение: подпись и адресат
        # приходят отдельно, чтобы можно было прикрепить одно фото к
        # разным чатам, не загружая дважды.
        return jsonify({'ok': True, 'media': {
            'id': media.id, 'size': media.size, 'mime': media.mime,
            'name': media.filename,
            'url': url_for('lorachat.media_get', mid=media.id),
            'expires': media.expires_at.isoformat() + 'Z',
        }})

    @bp.route('/api/media/<int:mid>')
    def media_get(mid):
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        media = db.session.get(LcMedia, mid)
        if not media or media.network_id != dev.network_id:
            abort(404)
        if not media.path or not os.path.exists(media.path):
            abort(410)   # протухло и вычищено
        import io
        with open(media.path, 'rb') as f:
            blob = sealer().unseal(media.network_id, f.read())
        return send_file(io.BytesIO(blob), mimetype=media.mime,
                         download_name=media.filename)

    @bp.route('/api/send', methods=['POST'])
    def api_send():
        """Отправка без WebSocket — для телефона в плохой сети и на
        случай, если сокет отвалился, а сообщение уже набрано."""
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        data = request.get_json(silent=True) or request.form
        result, err = _handle_send(dev, data)
        if err:
            return _err('send', err)
        return jsonify({'ok': True, **result})

    @bp.route('/api/lora_rx', methods=['POST'])
    def api_lora_rx():
        """То же, что op:lora_rx по сокету, но обычным POST.

        Телефон в плохой сети теряет сокет чаще, чем успевает его
        поднять, а услышанное в эфире терять нельзя: для получателя
        это может быть единственная копия сообщения.
        """
        dev = _device_by_token(_bearer())
        if not dev or dev.kind != 'phone':
            return _err('auth', 'Нужен токен телефона', 401)
        result, err = _handle_lora_rx(dev, request.get_json(silent=True) or {})
        if err:
            return _err('lora_rx', err)
        return jsonify({'ok': True, **result})

    @bp.route('/api/devices')
    def api_devices():
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        rows = LcSensor.query.filter_by(network_id=dev.network_id) \
                             .order_by(LcSensor.name).all()
        return jsonify({'ok': True, 'devices': [_sensor_json(r) for r in rows]})

    @bp.route('/api/devices/<node>/history')
    def api_device_history(node):
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        sensor = _sensor_by_node(dev.network_id, node)
        if not sensor:
            abort(404)
        try:
            hours = min(int(request.args.get('hours') or 24), 24 * 90)
            limit = min(int(request.args.get('limit') or 500), 2000)
        except ValueError:
            return _err('bad_args', 'hours и limit — числа')

        since = utcnow() - timedelta(hours=hours)
        rows = LcReading.query.filter(LcReading.sensor_id == sensor.id,
                                      LcReading.created_at >= since) \
                              .order_by(LcReading.created_at.desc()) \
                              .limit(limit).all()
        rows.reverse()
        out = []
        for r in rows:
            try:
                vals = json.loads(r.values_json or '{}')
            except ValueError:
                vals = {}
            out.append({'ts': r.created_at.isoformat() + 'Z',
                        'v': vals, 'rssi': r.rssi})
        return jsonify({'ok': True, 'device': sensor.node_id,
                        'fields': sensor.fields(), 'points': out})

    @bp.route('/api/buttons')
    def api_buttons():
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        member = db.session.get(LcMember, dev.member_id)
        rows = LcButton.query.filter_by(network_id=dev.network_id) \
                             .order_by(LcButton.sort, LcButton.id).all()
        return jsonify({'ok': True, 'buttons': [
            {'id': b.id, 'label': b.label, 'icon': b.icon, 'kind': b.kind,
             'confirm': bool(b.confirm), 'host_only': bool(b.host_only)}
            for b in rows if not b.host_only or member.is_host]})

    @bp.route('/api/stats')
    def api_stats():
        dev = _api_device()
        if not dev:
            return _err('auth', 'Нужен токен или вход в чат', 401)
        net = db.session.get(LcNetwork, dev.network_id)
        sensors = LcSensor.query.filter_by(network_id=net.id).count()
        return jsonify({'ok': True,
                        'online': sorted(hub.online_members(net.id)),
                        'used_bytes': net.used_bytes or 0,
                        'quota_bytes': net.quota_bytes or NET_QUOTA_BYTES,
                        'ttl_days': net.media_ttl_days or MEDIA_TTL_DAYS,
                        'reading_ttl_days': READING_TTL_DAYS,
                        'gateway': bool(hub.pick_gateway(net.id)),
                        'devices': sensors,
                        'call': bool(calls.for_network(net.id)),
                        'hub': hub.stats()})

    # ═══════════════════════════════════════════
    #            ОБРАБОТКА ОТПРАВКИ
    # ═══════════════════════════════════════════

    def _handle_send(dev, data):
        net = db.session.get(LcNetwork, dev.network_id)
        member = db.session.get(LcMember, dev.member_id)
        if not net or not member or not member.active:
            return None, 'участник отключён'

        kind = (data.get('kind') or 'text').lower()
        if kind not in ('text', 'voice', 'image', 'file', 'sos'):
            return None, 'неизвестный тип %s' % kind

        text = (data.get('text') or '')[:MAX_TEXT_CHARS]
        media_id = data.get('media_id')
        if media_id:
            media = db.session.get(LcMedia, int(media_id))
            if not media or media.network_id != net.id:
                return None, 'файл не найден'
        elif kind in ('voice', 'image', 'file'):
            return None, 'для %s нужен media_id' % kind
        if kind == 'text' and not text.strip():
            return None, 'пустое сообщение'

        target_kind = 'all'
        target_member_id = None
        tid = data.get('to')
        if tid:
            t = db.session.get(LcMember, int(tid))
            if not t or t.network_id != net.id:
                return None, 'получатель не из этой сети'
            target_kind, target_member_id = 'member', t.id

        if kind == 'sos':
            # SOS всегда всем: выбирать адресата в панике — плохая идея.
            target_kind, target_member_id = 'all', None
            if not text.strip():
                text = 'SOS'

        msg, created = _store_message(
            net, sender_member=member, sender_node=member.node_id,
            kind=kind, text=text, media_id=int(media_id) if media_id else None,
            target_kind=target_kind, target_member_id=target_member_id,
            origin='phone' if dev.kind == 'phone' else 'web')
        if not created:
            return {'msg_id': msg.id, 'duplicate': True}, None

        routes = _deliver(net, msg, member.id, dev.id)
        # Свои же другие вкладки/телефон должны увидеть отправленное.
        hub.send_to_member(net.id, member.id,
                           {'op': 'msg', 'msg': _msg_json(msg)},
                           exclude_device=dev.id)
        return {'msg_id': msg.id, 'routes': routes,
                'msg': _msg_json(msg)}, None

    def _handle_lora_rx(dev, data):
        """Телефон услышал пакет в эфире и зеркалит его в веб.

        Один пакет слышат несколько телефонов сразу, поэтому ключ
        дедупликации строится из узла-отправителя и packet_id — тех
        самых полей, по которым прошивка сама отличает повторы.
        """
        net = db.session.get(LcNetwork, dev.network_id)
        node = (data.get('node_id') or '').strip()[:16]
        if not node:
            return None, 'нет node_id'
        pkt = data.get('packet_id')
        dedup = 'lora:%s:%s:%s' % (net.id, node, pkt if pkt is not None else
                                   hashlib.sha1(
                                       (data.get('text') or '').encode()
                                   ).hexdigest()[:12])

        sender = LcMember.query.filter_by(network_id=net.id, node_id=node).first()
        kind = (data.get('kind') or 'text').lower()
        if kind not in ('text', 'voice', 'image', 'sos', 'sensor'):
            kind = 'text'

        msg, created = _store_message(
            net, sender_member=sender, sender_node=node,
            sender_name=(data.get('name') or (sender.title if sender else node)),
            kind=kind, text=(data.get('text') or '')[:MAX_TEXT_CHARS],
            origin='lora', dedup_key=dedup, lora_packet_id=pkt)
        if not created:
            return {'msg_id': msg.id, 'duplicate': True}, None

        # Пришедшее из эфира раздаём всем, кто сейчас в вебе. Обратно
        # в эфир НЕ отправляем: там оно уже прозвучало, и ретрансляцию
        # делает сам меш, а не мы.
        payload = {'op': 'msg', 'msg': _msg_json(msg)}
        hub.broadcast(net.id, payload)
        return {'msg_id': msg.id}, None

    # ═══════════════════════════════════════════
    #        АВТОНОМНЫЕ УСТРОЙСТВА И ВИДЖЕТЫ
    # ═══════════════════════════════════════════

    # История показаний нужна графику за последние недели, а не за все
    # годы: датчик с интервалом 60 с даёт полтора миллиона строк в год.
    READING_TTL_DAYS = 60
    # Чистим не по таймеру, а раз в N вставок: фонового потока в
    # gunicorn с gthread стоит избегать, а показания и так идут часто.
    READING_SWEEP_EVERY = 200
    _reading_counter = {'n': 0}

    def _sweep_readings():
        cutoff = utcnow() - timedelta(days=READING_TTL_DAYS)
        killed = LcReading.query.filter(LcReading.created_at < cutoff).delete(
            synchronize_session=False)
        if killed:
            current_app.logger.info('lorachat: удалено %d старых показаний', killed)
        return killed

    def _sensor_json(sensor):
        """Всё, что нужно клиенту, чтобы нарисовать виджет."""
        vals = sensor.values()
        out = {
            'node_id': sensor.node_id,
            'name': sensor.name,
            'class': sensor.dev_class,
            'interval': sensor.interval_s,
            'fields': sensor.fields(),
            'cmds': sensor.cmds(),
            'widget': sensor.widget(),
            'values': vals,
            'rssi': sensor.last_rssi,
            'last_seen': (sensor.last_seen_at.isoformat() + 'Z'
                          if sensor.last_seen_at else None),
        }
        # Датчик считается «пропавшим», если молчит дольше трёх своих
        # интервалов: одну пропущенную посылку в эфире ловить нормально,
        # три подряд — уже повод показать это человеку.
        if sensor.last_seen_at:
            silent = (utcnow() - sensor.last_seen_at).total_seconds()
            out['stale'] = silent > max(180, (sensor.interval_s or 60) * 3)
        else:
            out['stale'] = True
        out['derived'] = _eval_widget(sensor, vals)
        return out

    def _eval_widget(sensor, vals):
        """Посчитать производные значения виджета.

        Выражения безопасные (см. lorachat_expr): арифметика над
        показаниями и ничего больше. Ошибка в одном выражении не должна
        ронять виджет целиком — она превращается в null.
        """
        widget = sensor.widget()
        rows = widget.get('derived') or []
        if not rows:
            return []
        env = {}
        for f in sensor.fields():
            key = f.get('key') or f.get('name')
            if key:
                env[re.sub(r'\W', '_', key.lower())] = vals.get(str(f.get('i')), 0)
        out = []
        for row in rows[:12]:
            try:
                expr = _expr_cache_get(row.get('expr') or '')
                value = expr.eval(env) if expr else None
            except Exception:
                value = None
            out.append({'label': row.get('label') or '', 'value': value,
                        'unit': row.get('unit') or ''})
        return out

    _expr_cache = {}

    def _expr_cache_get(src):
        """Компилируем один раз: показания приходят часто, а выражение
        меняется раз в жизни."""
        if not src:
            return None
        hit = _expr_cache.get(src)
        if hit is None:
            try:
                hit = compile_expr(src)
            except ExprError:
                hit = False        # запомним и плохое, чтобы не парсить снова
            _expr_cache[src] = hit
        return hit or None

    def _sensor_by_node(net_id, node_id):
        return LcSensor.query.filter_by(network_id=net_id,
                                        node_id=node_id).first()

    def _handle_dev_hello(dev, data):
        """Манифест устройства, услышанный телефоном-шлюзом."""
        net = db.session.get(LcNetwork, dev.network_id)
        node = (data.get('node_id') or '').strip()[:16]
        if not node:
            return None, 'нет node_id'

        sensor = _sensor_by_node(net.id, node)
        if not sensor:
            sensor = LcSensor(network_id=net.id, node_id=node)
            db.session.add(sensor)
        sensor.name = (data.get('name') or node)[:60]
        sensor.dev_class = int(data.get('class') or 0)
        sensor.flags = int(data.get('flags') or 0)
        sensor.interval_s = int(data.get('interval') or 60)
        sensor.fields_json = json.dumps(data.get('fields') or [],
                                        ensure_ascii=False)
        sensor.cmds_json = json.dumps(data.get('cmds') or [], ensure_ascii=False)
        sensor.last_seen_at = utcnow()
        db.session.commit()

        hub.broadcast(net.id, {'op': 'dev_hello', 'device': _sensor_json(sensor)})
        return {'node_id': node}, None

    def _handle_dev_data(dev, data):
        net = db.session.get(LcNetwork, dev.network_id)
        node = (data.get('node_id') or '').strip()[:16]
        sensor = _sensor_by_node(net.id, node) if node else None
        if not sensor:
            # Показания от устройства, чей манифест мы ещё не слышали.
            # Заводим заглушку: числа покажем и так, имена подтянутся,
            # когда придёт манифест (он повторяется раз в N посылок).
            if not node:
                return None, 'нет node_id'
            sensor = LcSensor(network_id=net.id, node_id=node, name=node)
            db.session.add(sensor)

        vals = {}
        for item in (data.get('v') or [])[:16]:
            try:
                vals[str(int(item.get('i')))] = item.get('v')
            except (TypeError, ValueError):
                continue

        sensor.last_values = json.dumps(vals)
        sensor.last_rssi = data.get('rssi')
        sensor.last_seen_at = utcnow()
        db.session.add(LcReading(sensor_id=sensor.id, values_json=json.dumps(vals),
                                 rssi=data.get('rssi'), created_at=utcnow()))

        _reading_counter['n'] += 1
        if _reading_counter['n'] % READING_SWEEP_EVERY == 0:
            _sweep_readings()
            # Заодно выкидываем соединения, которые умерли молча: в
            # реестре они остаются «онлайн» и уводят сообщения в никуда.
            hub.sweep()
        db.session.commit()

        hub.broadcast(net.id, {'op': 'dev_data', 'node_id': node,
                               'values': vals, 'rssi': sensor.last_rssi,
                               'derived': _eval_widget(sensor, vals),
                               'ts': sensor.last_seen_at.isoformat() + 'Z'})
        return {'node_id': node}, None

    def _handle_dev_cmd(dev, data):
        """Команда устройству из веба: включить реле, опросить сейчас.

        Уходит она не сама — её выносит в эфир телефон-шлюз, как и
        обычное сообщение.
        """
        net = db.session.get(LcNetwork, dev.network_id)
        member = db.session.get(LcMember, dev.member_id)
        node = (data.get('node_id') or '').strip()[:16]
        sensor = _sensor_by_node(net.id, node) if node else None
        if not sensor:
            return None, 'устройство неизвестно'

        try:
            cmd_id = int(data.get('id'))
            arg = int(data.get('arg') or 0)
        except (TypeError, ValueError):
            return None, 'нужны id и arg'

        known = [c for c in sensor.cmds() if int(c.get('id', -1)) == cmd_id]
        if not known:
            return None, 'у устройства нет такой команды'

        gw = hub.pick_gateway(net.id, prefer_member_id=member.id)
        if not gw:
            return None, ('некому вынести команду в эфир — ни один телефон '
                          'не подключён шлюзом')

        gw.send({'op': 'dev_tx', 'node_id': node, 'id': cmd_id, 'arg': arg,
                 'by': member.title})
        # Команда на железо — не то, что должно происходить безымянно:
        # в чат уходит след, кто и что переключил.
        _store_message(net, sender_member=member, sender_node=member.node_id,
                       kind='sys', origin='web',
                       text='%s → %s' % (sensor.name, known[0].get('name') or cmd_id))
        _audit(net.id, 'dev_cmd', '%s#%d arg=%d by %s'
               % (node, cmd_id, arg, member.username))
        db.session.commit()
        return {'sent': True, 'node_id': node, 'id': cmd_id}, None

    def _handle_dev_ack(dev, data):
        net = db.session.get(LcNetwork, dev.network_id)
        hub.broadcast(net.id, {'op': 'dev_ack', 'node_id': data.get('node_id'),
                               'cmd': data.get('cmd'),
                               'status': data.get('status'),
                               'desc': (data.get('desc') or '')[:120]})
        return {'ok': True}, None

    def _handle_button(dev, data):
        """Нажата кнопка-плагин."""
        net = db.session.get(LcNetwork, dev.network_id)
        member = db.session.get(LcMember, dev.member_id)
        try:
            btn = db.session.get(LcButton, int(data.get('id')))
        except (TypeError, ValueError):
            return None, 'нужен id кнопки'
        if not btn or btn.network_id != net.id:
            return None, 'кнопки нет'
        if btn.host_only and not member.is_host:
            return None, 'кнопка только для хоста'

        if btn.kind == 'devcmd':
            return _handle_dev_cmd(dev, {'node_id': btn.node_id, 'id': btn.cmd_id,
                                         'arg': btn.cmd_arg})

        kind = 'sos' if btn.kind == 'sos' else 'text'
        result, err = _handle_send(dev, {'kind': kind,
                                         'text': btn.text or btn.label})
        if err:
            return None, err
        return result, None

    # ═══════════════════════════════════════════
    #                  ЗВОНКИ
    # ═══════════════════════════════════════════

    def _call_targets(call, exclude_member=None):
        """Кому раздавать звук: все веб-участники, кроме указанного."""
        return [mid for mid in call.web.keys() if mid != exclude_member]

    def _call_broadcast(call, payload, exclude_member=None):
        for mid in _call_targets(call, exclude_member):
            hub.send_to_member(call.network_id, mid, payload)

    def _call_state_out(call, note=None):
        st = call.state()
        if note:
            st['note'] = note
        return {'op': 'call_state', **st}

    def _air_participants(net, call):
        """Кто из участников сети доступен только по эфиру.

        Это ровно те, ради кого нужен FSK-мост: они не в вебе, но у них
        есть узел в эфире, и они слышны рацией.
        """
        out = []
        for m in LcMember.query.filter_by(network_id=net.id, active=True).all():
            if m.id in call.web or not m.node_id:
                continue
            if hub.is_online(net.id, m.id):
                continue
            out.append(m)
        return out

    def _call_open_air(net, call, initiator_device_id):
        """Поднять радио-плечо: найти шлюз и попросить его начать FSK.

        Возвращает текст проблемы или None. Проблему НЕ проглатываем:
        для звонящего разница между «Рома молчит» и «Рому физически
        некому позвать» принципиальная.
        """
        air = _air_participants(net, call)
        if not air:
            call.gateway_device = None
            return None

        gw = hub.pick_gateway(net.id, prefer_member_id=call.starter)
        if not gw:
            return ('в эфир звонок не уйдёт: ни один телефон не подключён '
                    'шлюзом, а %s нет в вебе'
                    % ', '.join(m.title for m in air))
        if not gw.can_gateway:
            return 'телефон-шлюз отказался от роли'

        call.gateway_device = gw.device_id
        dev = db.session.get(LcDevice, gw.device_id)
        if dev is not None and not dev.has_fsk:
            # Телефон подключён к плате без FSK (например, к E220):
            # текст она передаст, а голос — нет.
            call.gateway_device = None
            return ('у шлюза радио без FSK (%s) — голос в эфир не уйдёт, '
                    'только веб' % (dev.hw_profile or 'неизвестная плата'))

        for m in air:
            call.note_air_node(m.node_id, m.title)

        # Мост на телефоне работает с PCM: переводим весь звонок на него.
        # Иначе браузер продолжит слать Opus, а в эфир не уйдёт ничего —
        # и это была бы самая обидная тишина, без единой ошибки в логах.
        if call.codec != CODEC_PCM16:
            call.codec = CODEC_PCM16

        gw.send({
            'op': 'call_air_start',
            'call': call.id,
            'mode': call.mode,
            # Шлюз перекодирует: что придёт из веба и во что превращать.
            'web_codec': CODEC_NAMES[call.codec],
            'air_codec': CODEC_NAMES[CODEC_AMR_NB],
            'targets': [{'id': m.id, 'node': m.node_id, 'name': m.title}
                        for m in air],
        })
        return None

    def _handle_call_start(dev, data):
        # Отдельного фонового потока под уборку нет — она дешёвая и
        # делается здесь: звонок, из которого все выпали молча, не
        # должен мешать начать новый.
        for dead in calls.sweep():
            _close_air_leg(dead.network_id, dead)
            hub.broadcast(dead.network_id, {'op': 'call_end', 'call': dead.id})

        net = db.session.get(LcNetwork, dev.network_id)
        member = db.session.get(LcMember, dev.member_id)
        codec = int(data.get('codec') or CODEC_OPUS)
        mode = int(data.get('mode') or 1)
        if mode not in (0, 1, 2):
            mode = 1

        call, created = calls.start(net.id, member.id, dev.id, mode, codec)
        note = None
        if created:
            note = _call_open_air(net, call, dev.id)
            _store_message(net, sender_member=member, sender_node=member.node_id,
                           kind='sys', text='начал звонок', origin='web')

        payload = _call_state_out(call, note)
        # Приглашение уходит всем в сети, а не только участникам звонка:
        # иначе никто не узнает, что можно присоединиться.
        hub.broadcast(net.id, {'op': 'call_invite', 'call': call.id,
                               'from': member.id, 'from_name': member.title,
                               'bridged': bool(call.air)},
                      exclude_device=dev.id)
        _call_broadcast(call, payload)
        return payload, None

    def _handle_call_join(dev, data):
        net = db.session.get(LcNetwork, dev.network_id)
        codec = int(data.get('codec') or CODEC_OPUS)
        call = calls.join(net.id, dev.member_id, dev.id, codec)
        if not call:
            return None, 'звонок уже завершён'
        # Присоединившийся мог быть последним, кого не хватало в вебе —
        # радио-плечо пересобираем.
        note = _call_open_air(net, call, dev.id)
        payload = _call_state_out(call, note)
        _call_broadcast(call, payload)
        return payload, None

    def _close_air_leg(network_id, call):
        """Сказать шлюзу отпустить радио. Без этого плата осталась бы в
        FSK и оглохла для обычного LoRa до сработки watchdog."""
        if not call or not call.gateway_device:
            return
        for c in hub.conns(network_id):
            if c.device_id == call.gateway_device:
                c.send({'op': 'call_air_end', 'call': call.id})

    def _end_call(network_id, call):
        _close_air_leg(network_id, call)
        calls.end(network_id)
        hub.broadcast(network_id, {'op': 'call_end', 'call': call.id})

    def _handle_call_leave(dev, data):
        net = db.session.get(LcNetwork, dev.network_id)
        call, ended = calls.leave(net.id, dev.member_id)
        if not call:
            return {'op': 'call_state', 'call': None}, None
        if ended:
            _close_air_leg(net.id, call)
            hub.broadcast(net.id, {'op': 'call_end', 'call': call.id})
            return {'op': 'call_end', 'call': call.id}, None
        payload = _call_state_out(call)
        _call_broadcast(call, payload)
        return payload, None

    def _handle_call_floor(dev, data):
        """Запрос и отпускание слова для эфира."""
        net = db.session.get(LcNetwork, dev.network_id)
        call = calls.for_network(net.id)
        if not call:
            return None, 'звонка нет'
        if data.get('take'):
            ok = call.take_floor(dev.member_id)
            if not ok:
                holder = call.floor
                return {'op': 'call_floor', 'granted': False, 'holder': holder}, None
            _call_broadcast(call, {'op': 'call_floor', 'granted': True,
                                   'holder': call.floor})
            return {'op': 'call_floor', 'granted': True, 'holder': call.floor}, None
        call.release_floor(dev.member_id)
        _call_broadcast(call, {'op': 'call_floor', 'granted': False,
                               'holder': call.floor})
        return {'op': 'call_floor', 'granted': False, 'holder': call.floor}, None

    def _handle_audio(conn, dev, frame):
        """Раздать аудиокадр. Самая горячая точка модуля.

        В веб уходит всё и сразу — там полный дуплекс. В эфир уходит
        только речь того, кто держит слово: радио физически не может
        передавать двоих одновременно.
        """
        call = calls.for_network(dev.network_id)
        if not call:
            return
        if not frame['from_air'] and dev.member_id not in call.web:
            return
        call.last_audio_at = time.time()

        if frame['from_air']:
            # Пришло из эфира через шлюз. Отправителя в вебе нет, поэтому
            # member=0: клиент по этому признаку подпишет говорящего
            # «из эфира», а не пустым именем.
            if dev.id != call.gateway_device:
                return          # «из эфира» вправе прислать только шлюз
            out = pack_audio(frame['payload'], 0, frame['seq'], frame['codec'],
                             from_air=True)
            for mid in list(call.web.keys()):
                for c in hub.member_conns(call.network_id, mid):
                    c.send_bytes(out)
            return

        out = pack_audio(frame['payload'], dev.member_id, frame['seq'],
                         frame['codec'])
        for mid in _call_targets(call, exclude_member=dev.member_id):
            for c in hub.member_conns(call.network_id, mid):
                c.send_bytes(out)

        if not call.gateway_device:
            return
        # Слово берётся неявно, первым же кадром: заставлять человека
        # жать кнопку до начала фразы — верный способ терять первое слово.
        if not call.take_floor(dev.member_id):
            return
        for c in hub.conns(call.network_id):
            if c.device_id == call.gateway_device:
                c.send_bytes(out)

    # ═══════════════════════════════════════════
    #                WEBSOCKET
    # ═══════════════════════════════════════════

    def _ws_auth():
        """Кто на том конце сокета: телефон по токену или браузер по куке."""
        dev = _device_by_token(_bearer())
        if dev:
            return dev
        m = _session_member()
        if m:
            return _ensure_web_device(m)
        return None

    def _presence(net_id, member_id, online):
        hub.broadcast(net_id, {'op': 'presence', 'member': member_id,
                               'online': online,
                               'gateway': bool(hub.pick_gateway(net_id))})

    # Имя функции становится именем эндпоинта и для flask-sock, и для
    # обычного роута — так url_for('lorachat.ws_endpoint') работает
    # одинаково, поднят WebSocket на сервере или нет.
    def ws_endpoint(ws):
        dev = _ws_auth()
        if not dev:
            # Сокет уже открыт, поэтому отказ отдаём кадром, а не кодом
            # HTTP: клиент увидит причину, а не молчаливый разрыв.
            try:
                ws.send(json.dumps({'op': 'error', 'code': 'auth'}))
            except Exception:
                pass
            return

        member = db.session.get(LcMember, dev.member_id)
        net = db.session.get(LcNetwork, dev.network_id)
        if not member or not member.active or not net or not net.active:
            ws.send(json.dumps({'op': 'error', 'code': 'inactive'}))
            return

        conn = Conn(ws, net.id, member.id, dev.id, dev.kind,
                    bool(dev.can_gateway and dev.kind == 'phone'))
        hub.add(conn)
        dev.online = True
        dev.last_seen_at = utcnow()
        member.last_seen_at = utcnow()
        db.session.commit()

        conn.send({'op': 'hello_ok', 'member': member.id, 'net': net.slug,
                   'device': dev.id, 'kind': dev.kind,
                   'online': sorted(hub.online_members(net.id)),
                   'gateway': bool(hub.pick_gateway(net.id))})
        _presence(net.id, member.id, True)

        try:
            while True:
                raw = ws.receive()
                if raw is None:
                    break
                conn.last_rx = time.time()

                # Звук приходит бинарём и обрабатывается до разбора
                # JSON: это самый частый кадр на активном звонке, и
                # гонять его через json.loads незачем.
                if isinstance(raw, (bytes, bytearray)):
                    frame = parse_audio(raw)
                    if frame:
                        _handle_audio(conn, dev, frame)
                    continue

                try:
                    data = json.loads(raw)
                except (TypeError, ValueError):
                    conn.send({'op': 'error', 'code': 'bad_json'})
                    continue

                op = (data.get('op') or '').lower()

                if op == 'ping':
                    conn.send({'op': 'pong', 't': data.get('t')})

                elif op in ('msg', 'send'):
                    result, err = _handle_send(dev, data)
                    if err:
                        conn.send({'op': 'error', 'code': 'send', 'desc': err,
                                   'ref': data.get('ref')})
                    else:
                        conn.send({'op': 'sent', 'ref': data.get('ref'), **result})

                elif op == 'lora_rx':
                    result, err = _handle_lora_rx(dev, data)
                    if err:
                        conn.send({'op': 'error', 'code': 'lora_rx', 'desc': err})
                    else:
                        conn.send({'op': 'lora_rx_ok', **result})

                elif op == 'lora_tx_result':
                    # Телефон отчитался, что вынес сообщение в эфир.
                    mid = data.get('msg_id')
                    ok = bool(data.get('ok'))
                    if mid:
                        rows = LcDelivery.query.filter_by(message_id=int(mid),
                                                          via='lora').all()
                        for r in rows:
                            r.state = 'sent' if ok else 'failed'
                            r.updated_at = utcnow()
                        db.session.commit()
                        hub.broadcast(net.id, {'op': 'delivery', 'msg_id': int(mid),
                                               'via': 'lora',
                                               'state': 'sent' if ok else 'failed'})

                elif op == 'ack':
                    mid = data.get('msg_id')
                    if mid:
                        row = LcDelivery.query.filter_by(
                            message_id=int(mid), member_id=member.id).first()
                        if row:
                            row.state = data.get('state') or 'delivered'
                            row.updated_at = utcnow()
                            db.session.commit()

                elif op == 'typing':
                    hub.broadcast(net.id, {'op': 'typing', 'member': member.id,
                                           'on': bool(data.get('on'))},
                                  exclude_device=dev.id)

                elif op in ('dev_hello', 'dev_data', 'dev_ack'):
                    # Про эфир рассказывает только телефон: у вкладки
                    # браузера радио нет, и принимать от неё показания
                    # значило бы разрешить рисовать любые числа.
                    if dev.kind != 'phone':
                        conn.send({'op': 'error', 'code': 'not_a_gateway'})
                    else:
                        fn = {'dev_hello': _handle_dev_hello,
                              'dev_data': _handle_dev_data,
                              'dev_ack': _handle_dev_ack}[op]
                        out, err = fn(dev, data)
                        if err:
                            conn.send({'op': 'error', 'code': op, 'desc': err})

                elif op == 'dev_cmd':
                    out, err = _handle_dev_cmd(dev, data)
                    conn.send({'op': 'dev_cmd_ok', **out} if not err else
                              {'op': 'error', 'code': 'dev_cmd', 'desc': err})

                elif op == 'button':
                    out, err = _handle_button(dev, data)
                    conn.send({'op': 'button_ok', **(out or {})} if not err else
                              {'op': 'error', 'code': 'button', 'desc': err})

                elif op == 'call_start':
                    out, err = _handle_call_start(dev, data)
                    conn.send(out if not err else
                              {'op': 'error', 'code': 'call', 'desc': err})

                elif op == 'call_join':
                    out, err = _handle_call_join(dev, data)
                    conn.send(out if not err else
                              {'op': 'error', 'code': 'call', 'desc': err})

                elif op in ('call_leave', 'call_end'):
                    out, err = _handle_call_leave(dev, data)
                    conn.send(out if not err else
                              {'op': 'error', 'code': 'call', 'desc': err})

                elif op == 'call_floor':
                    out, err = _handle_call_floor(dev, data)
                    conn.send(out if not err else
                              {'op': 'error', 'code': 'call', 'desc': err})

                elif op == 'call_air_node':
                    # Шлюз сообщает, кого он слышит в эфире по этому
                    # звонку: узел мог войти в разговор, не имея вообще
                    # никакого отношения к вебу.
                    call = calls.for_network(net.id)
                    if call and dev.id == call.gateway_device:
                        call.note_air_node((data.get('node_id') or '')[:16],
                                           (data.get('name') or '')[:60] or None)
                        _call_broadcast(call, _call_state_out(call))

                elif op == 'gateway':
                    # Телефон может отказаться быть шлюзом (роуминг,
                    # низкий заряд) — уважаем и пересчитываем.
                    conn.can_gateway = bool(data.get('on')) and dev.kind == 'phone'
                    dev.can_gateway = conn.can_gateway
                    db.session.commit()
                    _presence(net.id, member.id, True)

                else:
                    conn.send({'op': 'error', 'code': 'unknown_op', 'desc': op})

        except Exception as e:                       # разрыв — норма
            current_app.logger.info('lorachat: сокет закрыт (%s)', e)
        finally:
            hub.remove(conn)
            # Оборванный сокет — тот же выход из звонка. Иначе человек
            # с севшим ноутбуком навсегда останется «в разговоре», а
            # если за ним было слово — заблокирует эфир остальным.
            try:
                left, ended = calls.leave(conn.network_id, conn.member_id)
                if left and ended:
                    _close_air_leg(conn.network_id, left)
                    hub.broadcast(conn.network_id,
                                  {'op': 'call_end', 'call': left.id})
                elif left:
                    _call_broadcast(left, _call_state_out(left))
            except Exception:
                current_app.logger.exception('lorachat: не смог убрать из звонка')

            dev = db.session.get(LcDevice, conn.device_id)
            if dev:
                dev.online = False
                dev.last_seen_at = utcnow()
                db.session.commit()
            _presence(conn.network_id, conn.member_id,
                      hub.is_online(conn.network_id, conn.member_id))

    if sock:
        sock.route('/api/ws', bp=bp)(ws_endpoint)
    else:
        # Без flask-sock чат остаётся рабочим, но без реалтайма:
        # клиент откатывается на /api/history и /api/send.
        bp.add_url_rule(
            '/api/ws', 'ws_endpoint',
            lambda: _err('no_ws', 'WebSocket на сервере не поднят '
                                  '(нужен пакет flask-sock)', 501))

    bp.lc_models = models
    bp.lc_hub = hub
    return bp
