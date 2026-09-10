"""Фикстуры тестов LoRa-чата.

Приложение создаётся ОДИН раз на весь прогон: фабрика блюпринта
объявляет модели SQLAlchemy, а объявить их дважды на одной базе нельзя
— второй раз классы переопределяют первые и метаданные разъезжаются.
В бою фабрика тоже зовётся ровно один раз, так что это не подгонка под
тест, а тот же режим работы.
"""

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


@pytest.fixture(scope='session')
def lc_app(tmp_path_factory):
    from lorachat_dev import create_app
    base = tmp_path_factory.mktemp('lorachat')
    return create_app(db_path=str(base / 'test.db'), instance=str(base / 'inst'))


@pytest.fixture
def lc_clean(lc_app):
    """Чистая база на каждый тест: тесты меняют квоты и отключают
    людей, и тащить это в соседний тест нельзя."""
    from lorachat_dev import db, DevUser, seed
    with lc_app.app_context():
        db.drop_all()
        db.create_all()
        db.session.add(DevUser(id=1, username='host'))
        db.session.commit()
    seed(lc_app)
    return lc_app
