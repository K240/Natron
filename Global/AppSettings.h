// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GLOBAL_APPSETTINGS_H
#define NATRON_GLOBAL_APPSETTINGS_H
#include <QCoreApplication>
#include <QSettings>
#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER
// QSettings(organization, application) always uses NativeFormat, even after
// setDefaultFormat(). Select the format explicitly for isolated test runs.
class AppSettings : public QSettings
{
public:
    explicit AppSettings(const QString& organization = QCoreApplication::organizationName(),
                         const QString& application = QCoreApplication::applicationName())
        : QSettings(qgetenv("NATRON_TEST_PROFILE").isEmpty() ? NativeFormat : IniFormat,
                    UserScope, organization, application)
    {
    }
};
NATRON_NAMESPACE_EXIT
#endif
