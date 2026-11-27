/*
 * nhl94_mode.h - the launcher's "Control mode" setting (Classic / Position
 * lock with seat roles). See nhl94_mode.c.
 */
#ifndef NHL94_MODE_H
#define NHL94_MODE_H

struct RecompLauncherCModProvider;

/* GameSpec.load_settings: read nhl94-control.ini beside settings.ini and
 * hand the seat list to nhl94_seats (lowest precedence). */
void nhl94_mode_load(const char *settings_ini);
/* Write nhl94-control.ini now (the build picks changed). */
void nhl94_mode_save(void);
/* GameSpec.mods: the engine's provider plus the Control mode feature. */
const struct RecompLauncherCModProvider *nhl94_mode_mods(const struct RecompLauncherCModProvider *base);

#endif /* NHL94_MODE_H */
