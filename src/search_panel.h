#pragma once
#include "search_session.h"
#include <QtWidgets>
#include <functional>
#include <limits>

namespace tatsu
{
class SearchEdit final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    bool composing = false;
    std::function<void(int)> submit;
    std::function<void()> leave;
    std::function<void()> compositionEnded;

protected:
    void inputMethodEvent(QInputMethodEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
};

class SearchPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPanel(QWidget* parent = nullptr);
    SearchEdit* editor() const
    {
        return query;
    }
    SearchSession* session() const
    {
        return results;
    }
    quint64 activeMatch() const
    {
        return active;
    }
    void setDocument(const PDFDocument* document, quint64 revision, int currentPage);
    void setCurrentPage(int page)
    {
        firstPage = page;
    }
    void next(int direction);
    void restoreActive(quint64 id);
signals:
    void matchActivated(const tatsu::SearchMatch& match);
    void presentationChanged();
    void returnToDocument();

private:
    void startSearch(int direction = 0);
    void schedule();
    void updateResults();
    void activate(int row);
    SearchEdit* query;
    SearchSession* results;
    QListView* list;
    QLabel* summary;
    QLabel* hint;
    QPushButton* previous;
    QPushButton* following;
    QTimer debounce;
    std::optional<PDFDocument> snapshot;
    quint64 revision = std::numeric_limits<quint64>::max();
    quint64 active = 0;
    int firstPage = 0, pendingDirection = 0;
    QString searched;
    QString notice;
};
} // namespace tatsu
