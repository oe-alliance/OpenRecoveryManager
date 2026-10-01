from os import remove
from os.path import exists
from pathlib import Path

from enigma import getDesktop

from Plugins.Plugin import PluginDescriptor
from Screens.MessageBox import MessageBox
from Screens.Standby import QUIT_RESTART, TryQuitMainloop

from . import _

ONCE = "/etc/enigma2/.orm-once"  # enigma2.sh starts ORM once instead of enigma2.


def title():
	return _("Recovery Manager")


def startRecoveryManager(session, **kwargs):
	def restartCancelled(restarting=False):  # No when a recording runs.
		if not restarting and exists(ONCE):
			remove(ONCE)

	def answered(answer):
		if answer:
			Path(ONCE).touch()
			session.openWithCallback(restartCancelled, TryQuitMainloop, QUIT_RESTART)

	session.openWithCallback(answered, MessageBox, _("Enigma2 stops and the Open Recovery Manager starts instead. Continue?"), MessageBox.TYPE_YESNO, default=False)


def supportMenu(menuid, **kwargs):
	return [(title(), startRecoveryManager, "recovery_manager", 50)] if menuid == "support" else []


def Plugins(**kwargs):
	return [
		PluginDescriptor(name=title(), description=_("Restarts Enigma2 into the Open Recovery Manager."), where=PluginDescriptor.WHERE_MENU, fnc=supportMenu, needsRestart=False),
		PluginDescriptor(name=title(), description=_("Restarts Enigma2 into the Open Recovery Manager."), where=PluginDescriptor.WHERE_PLUGINMENU, icon="plugin-fhd.png" if getDesktop(0).size().width() >= 1920 else "plugin.png", fnc=startRecoveryManager, needsRestart=False)
	]
