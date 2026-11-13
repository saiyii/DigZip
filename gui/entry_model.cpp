#include "entry_model.hpp"

#include <QDateTime>
#include <QFileIconProvider>
#include <QLocale>

#include <algorithm>

namespace digzip::gui {

QString format_size(std::uint64_t bytes)
{
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

EntryModel::EntryModel(QObject* parent)
    : QAbstractTableModel(parent),
      folder_icon_(QFileIconProvider().icon(QFileIconProvider::Folder)),
      file_icon_(QFileIconProvider().icon(QFileIconProvider::File))
{
}

void EntryModel::set_entries(std::vector<Entry> entries)
{
    beginResetModel();
    entries_ = std::move(entries);
    endResetModel();
}

bool EntryModel::any_encrypted() const
{
    return std::any_of(entries_.begin(), entries_.end(), [](const Entry& e) { return e.encrypted; });
}

int EntryModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int EntryModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

namespace {

QString type_label(EntryType type)
{
    switch (type) {
    case EntryType::Directory: return EntryModel::tr("folder");
    case EntryType::Symlink: return EntryModel::tr("link");
    case EntryType::Hardlink: return EntryModel::tr("hard link");
    case EntryType::Special: return EntryModel::tr("special file");
    case EntryType::File: break;
    }
    return {};
}

} // namespace

QVariant EntryModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount()) {
        return {};
    }
    const Entry& e = entries_[static_cast<std::size_t>(index.row())];
    const auto column = static_cast<Column>(index.column());

    if (role == Qt::DecorationRole && column == Name) {
        return e.type == EntryType::Directory ? folder_icon_ : file_icon_;
    }
    if (role == Qt::TextAlignmentRole && (column == Size || column == Packed)) {
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role == Qt::ToolTipRole && column == Name && !e.link_target.empty()) {
        return tr("Points to %1").arg(QString::fromUtf8(e.link_target.c_str()));
    }
    if (role != Qt::DisplayRole && role != SortRole) {
        return {};
    }
    const bool sorting = role == SortRole;

    switch (column) {
    case Name:
        return QString::fromUtf8(e.path.c_str());
    case Size:
        if (e.type != EntryType::File) {
            return sorting ? QVariant(qlonglong(-1)) : QVariant(type_label(e.type));
        }
        if (!e.size) {
            return sorting ? QVariant(qlonglong(-1)) : QVariant();
        }
        return sorting ? QVariant(static_cast<qlonglong>(*e.size)) : QVariant(format_size(*e.size));
    case Packed:
        if (!e.packed_size) {
            return sorting ? QVariant(qlonglong(-1)) : QVariant();
        }
        return sorting ? QVariant(static_cast<qlonglong>(*e.packed_size)) : QVariant(format_size(*e.packed_size));
    case Modified: {
        if (!e.mtime) {
            return sorting ? QVariant(qlonglong(0)) : QVariant();
        }
        if (sorting) {
            return QVariant(static_cast<qlonglong>(e.mtime->seconds));
        }
        const QDateTime time = QDateTime::fromSecsSinceEpoch(e.mtime->seconds);
        return QLocale().toString(time.toLocalTime(), QLocale::ShortFormat);
    }
    case Method:
        return QString::fromUtf8(e.method.c_str());
    case Encrypted:
        return e.encrypted ? tr("yes") : QString();
    case ColumnCount:
        break;
    }
    return {};
}

QVariant EntryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (static_cast<Column>(section)) {
    case Name: return tr("Name");
    case Size: return tr("Size");
    case Packed: return tr("Packed");
    case Modified: return tr("Modified");
    case Method: return tr("Method");
    case Encrypted: return tr("Encrypted");
    case ColumnCount: break;
    }
    return {};
}

} // namespace digzip::gui
