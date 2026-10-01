#ifndef RECOVERY_VERSION_H
#define RECOVERY_VERSION_H

#ifndef ORM_VERSION
#define ORM_VERSION "1.1"  /* A push of a new version to master tags and releases it as v<version>. */
#endif

#ifndef ORM_REVISION
#define ORM_REVISION ""  /* The git commit, set by the Makefile. */
#endif

#include <stdio.h>

/* "1.0 (8f917e5)", only "1.0" without the commit. */
static inline const char *orm_version(void)
{
	static char text[64];
	if (!ORM_REVISION[0])
		return ORM_VERSION;
	if (!text[0])
		snprintf(text, sizeof(text), "%s (%s)", ORM_VERSION, ORM_REVISION);
	return text;
}

#endif
