#pragma once

#include <digzip/entry.hpp>

#include <QAbstractTableModel>
#include <QIcon>

#include <vector>

namespace digzip::gui {

// The entries of the open archive, one row each.
class EntryModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { Name, Size, Packed, Modified, Method, Encrypted, ColumnCount };
    static constexpr int SortRole = Qt::UserRole; // raw value used for sorting

    explicit EntryModel(QObject* parent = nullptr);

    void set_entries(std::vector<Entry> entries);
    const std::vector<Entry>& entries() const { return entries_; }
    bool any_encrypted() const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    std::vector<Entry> entries_;
    QIcon folder_icon_;
    QIcon file_icon_;
};

QString format_size(std::uint64_t bytes);

} // namespace digzip::gui
