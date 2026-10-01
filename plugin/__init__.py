from gettext import bindtextdomain, dgettext, gettext

from Components.Language import language

# The catalogs of ORM, <language>/LC_MESSAGES/orm.mo, like LOCALEDIR of the Makefile.
PluginLanguageDomain = "orm"
PluginLanguagePath = "/usr/share/locale"


def localeInit():
	bindtextdomain(PluginLanguageDomain, PluginLanguagePath)


def _(text):
	translated = dgettext(PluginLanguageDomain, text)
	return translated if translated != text else gettext(text)


localeInit()
language.addCallback(localeInit)
