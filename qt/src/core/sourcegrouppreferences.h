#pragma once

#include <QStringList>
#include <QSet>

namespace OKILTV::Core {

struct SourceGroupPreferences
{
    QStringList hiddenGroups;
    QStringList groupOrder;
    bool autoEnableSkipped { false };
};

// discoveredIds contains normalized, unique IDs of groups with channels (plus Favourites).
inline SourceGroupPreferences reconcileSourceGroups(
    const QStringList &discoveredIds,
    const SourceGroupPreferences &persisted)
{
    const auto favouritesId = QStringLiteral("__favourites__");
    const auto sourceGroupCount = discoveredIds.size() - (discoveredIds.contains(favouritesId) ? 1 : 0);
    const auto autoEnable = sourceGroupCount <= 50;
    SourceGroupPreferences result { persisted.hiddenGroups, persisted.groupOrder, false };
    QSet<QString> known(persisted.groupOrder.cbegin(), persisted.groupOrder.cend());
    known.unite(QSet<QString>(persisted.hiddenGroups.cbegin(), persisted.hiddenGroups.cend()));
    for (const auto &id : discoveredIds) {
        if (!known.contains(id) && id != favouritesId && !autoEnable) {
            result.hiddenGroups.push_back(id);
            result.autoEnableSkipped = true;
        }
        if (!result.groupOrder.contains(id)) {
            if (id == favouritesId) {
                result.groupOrder.prepend(id);
            } else {
                result.groupOrder.push_back(id);
            }
        }
    }
    return result;
}

} // namespace OKILTV::Core
