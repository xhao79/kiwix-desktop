#include "contentmanagermodel.h"
#include "node.h"
#include "rownode.h"
#include "descriptionnode.h"
#include <zim/error.h>
#include <zim/item.h>
#include "kiwixapp.h"
#include <kiwix/tools.h>

ContentManagerModel::ContentManagerModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    connect(&td, &ThumbnailDownloader::oneThumbnailDownloaded, this, &ContentManagerModel::updateImage);
}

ContentManagerModel::~ContentManagerModel()
{
}

int ContentManagerModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return static_cast<Node*>(parent.internalPointer())->columnCount();
    return rootNode->columnCount();
}

QVariant ContentManagerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    auto item = static_cast<Node*>(index.internalPointer());
    const auto displayRole = role == Qt::DisplayRole;
    const auto additionalInfoRole = role == Qt::UserRole+1;
    if (displayRole || additionalInfoRole)
        return item->data(index.column());

    return QVariant();
}

Qt::ItemFlags ContentManagerModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);
    if (index.isValid() && index.parent().isValid()) {
        return defaultFlags & ~Qt::ItemIsDropEnabled & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsSelectable & ~Qt::ItemIsEditable & ~Qt::ItemIsUserCheckable;
    }
    return defaultFlags;
}

QModelIndex ContentManagerModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();

    RowNode* parentItem;

    if (!parent.isValid()) {
        parentItem = rootNode.get();
    }
    else {
        parentItem = static_cast<RowNode*>(parent.internalPointer());
    }
    auto childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem.get());

    return QModelIndex();
}

QModelIndex ContentManagerModel::parent(const QModelIndex &index) const
{
    if (!index.isValid())
        return QModelIndex();

    auto childItem = static_cast<Node*>(index.internalPointer());
    auto parentItem = childItem->parentItem();

    if (!parentItem || parentItem == rootNode)
        return QModelIndex();

    return createIndex(parentItem->row(), 0, parentItem.get());
}

int ContentManagerModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return zimCount;
}

QVariant ContentManagerModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return QVariant();

    switch (section)
    {
        case 1: return "Name";
        case 2: return "Date";
        case 3: return "Size";
        case 4: return "Content Type";
        default: return QVariant();
    }
}

void ContentManagerModel::setBooksData(const BookInfoList& data)
{
    m_data = data;
    rootNode = std::shared_ptr<RowNode>(new RowNode({tr("Icon"), tr("Name"), tr("Date"), tr("Size"), tr("Content Type"), tr("Download")}, "", std::weak_ptr<RowNode>()));
    setupNodes();
    emit dataChanged(QModelIndex(), QModelIndex());
}

std::shared_ptr<RowNode> ContentManagerModel::createNode(BookInfo bookItem, QMap<QString, QByteArray> iconMap) const
{
    const auto faviconUrl = bookItem["faviconUrl"].toString();
    QString id = bookItem["id"].toString();
    QByteArray bookIcon;
    try {
        auto book = KiwixApp::instance()->getLibrary()->getBookById(id);
        std::string favicon;
        auto item = book.getIllustration(48);
        favicon = item->getData();
        bookIcon = QByteArray::fromRawData(reinterpret_cast<const char*>(favicon.data()), favicon.size());
        bookIcon.detach(); // deep copy
    } catch (...) {
        if (iconMap.contains(faviconUrl)) {
            bookIcon = iconMap[faviconUrl];
        }
    }
    std::weak_ptr<RowNode> weakRoot = rootNode;
    auto rowNodePtr = std::shared_ptr<RowNode>(new
                                    RowNode({bookIcon, bookItem["title"],
                                   bookItem["date"],
                                   QString::fromStdString(kiwix::beautifyFileSize(bookItem["size"].toULongLong())),
                                   bookItem["tags"]
                                   }, id, weakRoot));
    std::weak_ptr<RowNode> weakRowNodePtr = rowNodePtr;
    const auto descNodePtr = std::make_shared<DescriptionNode>(DescriptionNode(bookItem["description"].toString(), weakRowNodePtr));

    rowNodePtr->appendChild(descNodePtr);
    return rowNodePtr;
}

void ContentManagerModel::setupNodes()
{
    beginResetModel();
    bookIdToRowMap.clear();
    for (auto bookItem : m_data) {
        const auto rowNode = createNode(bookItem, iconMap);

        // Restore download state during model updates (filtering, etc)
        const auto downloadIter = m_downloads.constFind(rowNode->getBookId());
        if ( downloadIter != m_downloads.constEnd() ) {
            rowNode->setDownloadState(downloadIter.value());
        }

        bookIdToRowMap[bookItem["id"].toString()] = rootNode->childCount();
        rootNode->appendChild(rowNode);
    }
    endResetModel();
}

void ContentManagerModel::refreshIcons()
{
    if (KiwixApp::instance()->getContentManager()->isLocal())
        return;
    td.clearQueue();
    for (auto i = 0; i < rowCount() && i < m_data.size(); i++) {
        auto bookItem = m_data[i];
        auto id = bookItem["id"].toString();
        const auto faviconUrl = bookItem["faviconUrl"].toString();
        auto app = KiwixApp::instance();
        try {
            auto book = app->getLibrary()->getBookById(id);
            auto item = book.getIllustration(48);
        } catch (...) {
            if (faviconUrl != "" && !iconMap.contains(faviconUrl)) {
                td.addDownload(faviconUrl, id);
            }
        }
    }
}

bool ContentManagerModel::hasChildren(const QModelIndex &parent) const
{
    auto item = static_cast<Node*>(parent.internalPointer());
    if (item)
        return item->childCount() > 0;
    return true;
}

bool ContentManagerModel::canFetchMore(const QModelIndex &parent) const
{
    if (parent.isValid())
        return false;
    return (zimCount < m_data.size());
}

void ContentManagerModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid())
        return;
    int remainder = m_data.size() - zimCount;
    int zimsToFetch = qMin(5, remainder);
    beginInsertRows(QModelIndex(), zimCount, zimCount + zimsToFetch - 1);
    zimCount += zimsToFetch;
    endInsertRows();
    refreshIcons();
}

void ContentManagerModel::sort(int column, Qt::SortOrder order)
{
    if (column == 0 || column == 4 || column == 5)
        return;

    QString sortBy = "";
    switch(column) {
        case 1:
            sortBy = "title";
            break;
        case 2:
            sortBy = "date";
            break;
        case 3:
            sortBy = "size";
            break;
        default:
            sortBy = "unsorted";
    }
    KiwixApp::instance()->getContentManager()->setSortBy(sortBy, order == Qt::AscendingOrder);
}

void ContentManagerModel::updateImage(QString bookId, QString url, QByteArray imageData)
{
    const auto it = bookIdToRowMap.constFind(bookId);
    if ( it == bookIdToRowMap.constEnd() )
        return;

    const size_t row = it.value();
    const auto item = static_cast<RowNode*>(rootNode->child(row).get());
    item->setIconData(imageData);
    iconMap[url] = imageData;
    const QModelIndex index = this->index(row, 0);
    emit dataChanged(index, index);
}

std::shared_ptr<RowNode> getSharedPointer(RowNode* ptr)
{
    return std::static_pointer_cast<RowNode>(ptr->shared_from_this());
}

void ContentManagerModel::startDownload(QModelIndex index)
{
    auto node = getSharedPointer(static_cast<RowNode*>(index.internalPointer()));
    const auto bookId = node->getBookId();
    const auto newDownload = std::make_shared<DownloadState>();
    m_downloads[bookId] = newDownload;
    node->setDownloadState(newDownload);
    QTimer *timer = newDownload->getDownloadUpdateTimer();
    connect(timer, &QTimer::timeout, this, [=]() {
        updateDownload(bookId);
    });
}

void ContentManagerModel::updateDownload(QString bookId)
{
    const auto download = m_downloads.value(bookId);

    if ( ! download )
        return;

    const bool downloadStillValid = download->update(bookId);

    // The download->update() call above may result in
    // ContentManagerModel::setBooksData() being called (through a chain
    // of signals), which in turn will rebuild bookIdToRowMap. Hence
    // bookIdToRowMap access must happen after it.

    const auto it = bookIdToRowMap.constFind(bookId);

    if ( ! downloadStillValid ) {
        m_downloads.remove(bookId);
        if ( it != bookIdToRowMap.constEnd() ) {
            const size_t row = it.value();
            RowNode& rowNode = static_cast<RowNode&>(*rootNode->child(row));
            rowNode.setDownloadState(nullptr);
        }
    }

    if ( it != bookIdToRowMap.constEnd() ) {
        const size_t row = it.value();
        const QModelIndex rootNodeIndex = this->index(0, 0);
        const QModelIndex newIndex = this->index(row, 5, rootNodeIndex);
        emit dataChanged(newIndex, newIndex);
    }
}

void ContentManagerModel::pauseDownload(QModelIndex index)
{
    auto node = static_cast<RowNode*>(index.internalPointer());
    node->getDownloadState()->pause();
    emit dataChanged(index, index);
}

void ContentManagerModel::resumeDownload(QModelIndex index)
{
    auto node = static_cast<RowNode*>(index.internalPointer());
    node->getDownloadState()->resume();
    emit dataChanged(index, index);
}

void ContentManagerModel::cancelDownload(QModelIndex index)
{
    auto node = static_cast<RowNode*>(index.internalPointer());
    node->setDownloadState(nullptr);
    m_downloads.remove(node->getBookId());
    emit dataChanged(index, index);
}

