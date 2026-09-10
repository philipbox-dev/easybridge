"""Автономный запуск LoRa-чата — без app.py и без прода.

Поднимает минимальное Flask-приложение с одним блюпринтом lorachat,
поддельным flask-login и временной базой. Нужен для разработки и
тестов: трогать живой meetjimproviz.ru ради проверки формы логина
нельзя, а зависимостей у модуля ровно столько, сколько тут собрано.

    python lorachat_dev.py            # http://127.0.0.1:5055/lora-chat/
    python lorachat_dev.py --seed     # + тестовая сеть и участники
"""

import argparse
import os
import sys

from flask import Flask, redirect, request, url_for
from flask_login import LoginManager, UserMixin, login_user
from flask_sqlalchemy import SQLAlchemy

db = SQLAlchemy()


class DevUser(UserMixin, db.Model):
    __tablename__ = 'dev_user'
    id = db.Column(db.Integer, primary_key=True)
    username = db.Column(db.String(50), default='host')


def create_app(db_path=None, instance=None):
    app = Flask(__name__, instance_path=instance or
                os.path.join(os.path.dirname(os.path.abspath(__file__)), 'instance'))
    app.config.update(
        SECRET_KEY='dev-only-not-a-secret',
        SQLALCHEMY_DATABASE_URI='sqlite:///' + (db_path or
                                                os.path.join(app.instance_path,
                                                             'lorachat_dev.db')),
        SQLALCHEMY_TRACK_MODIFICATIONS=False,
        WTF_CSRF_ENABLED=False,       # в деве форм-токены только мешают
        SERVER_NAME=None,
    )
    os.makedirs(app.instance_path, exist_ok=True)
    db.init_app(app)

    # Страницы хоста наследуются от layout.html настоящего сайта, а он
    # тянет за собой пол-app.py. Для автономного запуска подменяем его
    # минимальной обёрткой: проверяем свои шаблоны, а не чужую вёрстку.
    from jinja2 import ChoiceLoader, DictLoader
    app.jinja_loader = ChoiceLoader([
        DictLoader({'layout.html':
                    '<!doctype html><meta charset="utf-8">'
                    '<title>dev</title><body>{% block content %}{% endblock %}'}),
        app.jinja_loader,
    ])
    app.jinja_env.globals.setdefault('csrf_token', lambda: 'dev')

    lm = LoginManager(app)
    lm.login_view = 'login'

    @lm.user_loader
    def load_user(uid):
        return db.session.get(DevUser, int(uid))

    # Заглушка логина: настоящий сайт делает это сам, здесь важно
    # только то, что current_user.id совпадает с owner_id сети.
    @app.route('/login')
    def login():
        u = db.session.get(DevUser, 1)
        login_user(u)
        return redirect(request.args.get('next') or url_for('lorachat.index'))

    try:
        from flask_sock import Sock
        sock = Sock(app)
    except ImportError:
        sock = None
        app.logger.warning('flask-sock не установлен — WebSocket отключён')

    from lorachat import create_lorachat_blueprint
    bp = create_lorachat_blueprint(db, csrf=None, sock=sock)
    app.register_blueprint(bp)

    with app.app_context():
        db.create_all()
        if not db.session.get(DevUser, 1):
            db.session.add(DevUser(id=1, username='host'))
            db.session.commit()

    app.lc_models = bp.lc_models
    app.lc_hub = bp.lc_hub
    return app


def seed(app):
    """Тестовая сеть: Philip (хост), Misha, Roma — как в сценарии из ТЗ."""
    from werkzeug.security import generate_password_hash
    from lorachat import gen_uid
    M = app.lc_models
    with app.app_context():
        net = M['Network'].query.filter_by(slug='test').first()
        if net:
            return net
        net = M['Network'](owner_id=1, name='Тест', slug='test')
        db.session.add(net)
        db.session.commit()
        made = []
        for name, node, host in (('philip', '0xAAAA0001', True),
                                 ('misha', '0xAAAA0002', False),
                                 ('roma', '0xAAAA0003', False)):
            m = M['Member'](network_id=net.id, uid=gen_uid(), username=name,
                            display_name=name.capitalize(), node_id=node,
                            is_host=host,
                            password_hash=generate_password_hash('test1234'))
            db.session.add(m)
            made.append(m)
        db.session.commit()
        return net


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--seed', action='store_true')
    ap.add_argument('--port', type=int, default=5055)
    args = ap.parse_args()
    app = create_app()
    if args.seed:
        net = seed(app)
        with app.app_context():
            for m in app.lc_models['Member'].query.filter_by(
                    network_id=net.id).all():
                print('%-8s пароль test1234   /lora-chat/%s/chat'
                      % (m.username, m.uid))
    app.run(host='127.0.0.1', port=args.port, debug=False, threaded=True)
