#ifndef RECOVERY_VERSION_H
#define RECOVERY_VERSION_H

#ifndef ORM_VERSION
#define ORM_VERSION "1.5"
#endif

#ifndef ORM_REVISION
#define ORM_REVISION ""
#endif

#include <stdio.h>

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
