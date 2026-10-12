"""Read the user's Natron preferences without exposing their values."""
import hashlib
import json

from PySide6.QtCore import QSettings

settings = QSettings(QSettings.NativeFormat, QSettings.UserScope, "INRIA", "Natron")
values = {key: settings.value(key) for key in settings.allKeys()}
print(hashlib.sha256(json.dumps(values, sort_keys=True, default=str).encode()).hexdigest())
