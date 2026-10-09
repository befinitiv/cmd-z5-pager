#include <string.h>

#include <zephyr/sys/util.h>

#include "i18n.h"

struct translation {
	const char *en;
	const char *text;
};

/*
 * German translations. Keep the texts short: a menu row holds about 18
 * characters, the small font on the status and hint lines 24.
 */
static const struct translation de[] = {
	/* Main menu */
	{"Menu", "Menü"},
	{"Messages", "Nachrichten"},
	{"Nodes", "Knoten"},
	{"Settings", "Einstellungen"},
	{"Hardware test", "Hardware-Test"},
	{"About", "Info"},
	{"%s is not implemented yet. It follows in a later phase.",
	 "%s ist noch nicht implementiert. Folgt in einer späteren Phase."},

	/* Home */
	{"time not set", "Zeit nicht gesetzt"},
	{"Mesh off", "Mesh aus"},
	{"push: Menu", "Drücken: Menü"},

	/* Settings */
	{"Display", "Display"},
	{"Sound", "Töne"},
	{"Text input", "Texteingabe"},
	{"Language", "Sprache"},
	{"Contrast", "Kontrast"},
	{"Backlight", "Beleuchtung"},
	{"Light off", "Licht aus"},
	{"Screen off", "Display aus"},
	{"Volume", "Lautstärke"},
	{"Key clicks", "Tastentöne"},
	{"Vibration", "Vibration"},
	{"on", "an"},
	{"off", "aus"},
	{"never", "nie"},
	{"Learn words", "Wörter lernen"},
	{"Delete learned", "Gelerntes löschen"},
	{"Learned words deleted.", "Gelernte Wörter gelöscht."},

	/* Messages */
	{"New message", "Neue Nachricht"},
	{"Inbox", "Posteingang"},
	{"CALL = send", "CALL = senden"},
	{"wheel: move  *: done", "Rad: bewegen *: fertig"},
	{"Sending messages follows in phase 3.", "Das Senden folgt in Phase 3."},

	/* Widgets */
	{"push = ok   C = cancel", "Drücken = OK  C = Abbr."},

	/* Hardware test */
	{"Blink LEDs", "LEDs blinken"},
	{"Vibrate", "Vibrieren"},
	{"Speaker 1 kHz", "Lautsprecher 1 kHz"},
	{"Earpiece 1 kHz", "Hörer 1 kHz"},
	{"Melody speaker", "Melodie Lautspr."},
	{"Melody earpiece", "Melodie Hörer"},
	{"Key test", "Tastentest"},
	{"Display test", "Displaytest"},
	{"Event: %s", "Ereignis: %s"},
	{"Key: %s", "Taste: %s"},
	{"Value: %+d", "Wert: %+d"},
	{"Wheel: %d", "Rad: %d"},
	{"Events: %d", "Ereignisse: %d"},
	{"Press a key", "Taste drücken"},
	{"hold C/bend up: exit", "C/zurück halten: Ende"},
	{"press", "gedrückt"},
	{"long", "lang"},
	{"release", "los"},
	{"wheel", "Rad"},

	/* About */
	{"CMD-Z5 firmware %s\nZephyr %s\nBuilt %s\n\n"
	 "Meshtastic and LoRaWAN messenger for the Sony CMD-Z5.",
	 "CMD-Z5 Firmware %s\nZephyr %s\nErstellt %s\n\n"
	 "Meshtastic- und LoRaWAN-Messenger für das Sony CMD-Z5."},

	/* Mesh */
	{"Mesh", "Mesh"},
	{"Name", "Name"},
	{"Short name", "Kurzname"},
	{"Region", "Region"},
	{"Preset", "Profil"},
	{"Node", "Knoten"},
	{"sending", "sendet"},
	{"sent", "gesendet"},
	{"delivered", "zugestellt"},
	{"no relay", "kein Relais"},
	{"failed", "fehlgeschl."},
	{"No messages", "Keine Nachrichten"},
	{"push: write", "Drücken: schreiben"},
	{"No nodes heard yet", "Noch keine Knoten"},
	{"Radio not available", "Funk nicht verfügbar"},
	{"Heard", "Gehört"},
	{"Hops", "Sprünge"},
	{"Encrypted DMs", "Verschlüsselte DMs"},
	{"No key (channel DMs)", "Kein Schlüssel"},
	{"push: message", "Drücken: Nachricht"},
	{"CALL = save", "CALL = speichern"},
	{"%d new message", "%d neue Nachricht"},
	{"%d new messages", "%d neue Nachrichten"},
	{"%d node", "%d Knoten"},
	{"%d nodes", "%d Knoten"},
	{"The radio is not available.", "Der Funk ist nicht verfügbar."},

	/* Power */
	{"Power off", "Ausschalten"},
	{"Release the key to switch off.", "Taste loslassen zum Ausschalten."},
	{"Goodbye", "Auf Wiedersehen"},
};

static const struct translation *const tables[LANG_COUNT] = {
	[LANG_EN] = NULL,
	[LANG_DE] = de,
};

static const size_t table_sizes[LANG_COUNT] = {
	[LANG_DE] = ARRAY_SIZE(de),
};

static enum lang current = LANG_EN;

void i18n_set(enum lang lang)
{
	current = lang < LANG_COUNT ? lang : LANG_EN;
}

enum lang i18n_get(void)
{
	return current;
}

const char *tr(const char *en)
{
	const struct translation *t = tables[current];

	for (size_t i = 0; t && i < table_sizes[current]; i++) {
		if (strcmp(t[i].en, en) == 0) {
			return t[i].text;
		}
	}
	return en;
}

const char *i18n_name(enum lang lang)
{
	return lang == LANG_DE ? "Deutsch" : "English";
}
