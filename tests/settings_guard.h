#pragma once
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <utility>

namespace tatsu::test
{
class SettingsGuard
{
public:
    explicit SettingsGuard(QStringList selectedKeys) : keys(std::move(selectedKeys))
    {
        QSettings settings;
        for (const auto& key : keys)
            if (settings.contains(key))
                values.insert(key, settings.value(key));
    }
    ~SettingsGuard()
    {
        QSettings settings;
        for (const auto& key : keys)
            if (values.contains(key))
                settings.setValue(key, values.value(key));
            else
                settings.remove(key);
    }
    SettingsGuard(const SettingsGuard&) = delete;
    SettingsGuard& operator=(const SettingsGuard&) = delete;

private:
    QStringList keys;
    QMap<QString, QVariant> values;
};
} // namespace tatsu::test
