"""Owned GUI regression: destroy open node panels with a populated clipboard.

The runner must check the process exit code as well as the assertion report.
"""
import json
import os
import traceback
from pathlib import Path

from PySide6.QtCore import QMetaObject, QTimer, Qt
from PySide6.QtWidgets import QApplication
import NatronGui


def finish():
    report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE']}
    try:
        application = NatronGui.PyGuiApplication().getGuiInstance(app.getAppID())
        card = application.createNode('fr.inria.built-in.Card')
        application.setSelection([card])
        graphs = [w for w in QApplication.allWidgets() if w.metaObject().className() == 'Natron::NodeGraph' and w.isVisible()]
        assert len(graphs) == 1
        assert QMetaObject.invokeMethod(graphs[0], 'copySelectedNodes', Qt.DirectConnection)
        report['ok'] = True
    except Exception as error:
        report.update(error=str(error), traceback=traceback.format_exc())
    Path(os.environ['NATRON_GUI_TEST_RESULT']).write_text(json.dumps(report, indent=2), encoding='utf-8')
    QApplication.exit(0)


QTimer.singleShot(700, finish)
