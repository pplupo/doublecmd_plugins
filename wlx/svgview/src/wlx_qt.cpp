#include "svg_core.h"
#include "wlxplugin.h"
#include <QAbstractScrollArea>
#include <QPainter>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QScrollBar>
#include <QImage>

class SvgWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    SvgWidget(const QString& file_path, QWidget* parent = nullptr) 
        : QAbstractScrollArea(parent), m_dragging(false) 
    {
        setFocusPolicy(Qt::StrongFocus);
        m_core.load(file_path.toUtf8().constData());
        
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &SvgWidget::onScrollbarChanged);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &SvgWidget::onScrollbarChanged);
    }

    void onScrollbarChanged() {
        m_core.set_pan(horizontalScrollBar()->value(), verticalScrollBar()->value());
        viewport()->update();
    }

    void updateScrollbars() {
        double doc_w = m_core.get_intrinsic_width() * m_core.get_zoom();
        double doc_h = m_core.get_intrinsic_height() * m_core.get_zoom();
        
        horizontalScrollBar()->setPageStep(viewport()->width());
        horizontalScrollBar()->setRange(0, qMax(0.0, doc_w - viewport()->width()));
        horizontalScrollBar()->setValue(m_core.get_pan_x());

        verticalScrollBar()->setPageStep(viewport()->height());
        verticalScrollBar()->setRange(0, qMax(0.0, doc_h - viewport()->height()));
        verticalScrollBar()->setValue(m_core.get_pan_y());
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        int w = viewport()->width();
        int h = viewport()->height();
        
        m_core.render(w, h);
        
        const unsigned char* pixels = m_core.get_image_data();
        if (pixels) {
            QImage img(pixels, w, h, m_core.get_image_stride(), QImage::Format_ARGB32_Premultiplied);
            QPainter p(viewport());
            p.drawImage(0, 0, img);
        }
    }

    void wheelEvent(QWheelEvent* event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            double factor = (event->angleDelta().y() > 0) ? 1.1 : 0.9;
            m_core.zoom_delta(factor, event->position().x(), event->position().y());
            updateScrollbars();
            viewport()->update();
        } else {
            QAbstractScrollArea::wheelEvent(event);
        }
    }

    void keyPressEvent(QKeyEvent* event) override {
        if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_S) {
            savePng();
            return;
        }
        
        if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
            m_core.zoom_delta(1.1, viewport()->width()/2.0, viewport()->height()/2.0);
            updateScrollbars();
            viewport()->update();
        } else if (event->key() == Qt::Key_Minus) {
            m_core.zoom_delta(0.9, viewport()->width()/2.0, viewport()->height()/2.0);
            updateScrollbars();
            viewport()->update();
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_last_pos = event->pos();
        } else if (event->button() == Qt::RightButton) {
            QMenu menu(this);
            QAction* saveAction = menu.addAction("Save as PNG...");
            connect(saveAction, &QAction::triggered, this, &SvgWidget::savePng);
            menu.exec(event->globalPosition().toPoint());
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_dragging = false;
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_dragging) {
            double dx = event->pos().x() - m_last_pos.x();
            double dy = event->pos().y() - m_last_pos.y();
            m_last_pos = event->pos();

            m_core.set_pan(m_core.get_pan_x() - dx, m_core.get_pan_y() - dy);
            
            QSignalBlocker block_h(horizontalScrollBar());
            QSignalBlocker block_v(verticalScrollBar());
            horizontalScrollBar()->setValue(m_core.get_pan_x());
            verticalScrollBar()->setValue(m_core.get_pan_y());
            
            viewport()->update();
        }
    }

    void resizeEvent(QResizeEvent* event) override {
        updateScrollbars();
        QAbstractScrollArea::resizeEvent(event);
    }

private:
    void savePng() {
        QString fileName = QFileDialog::getSaveFileName(this, "Save PNG", "export.png", "Images (*.png)");
        if (!fileName.isEmpty()) {
            m_core.save_png(fileName.toUtf8().constData());
        }
    }

    SvgCore m_core;
    bool m_dragging;
    QPoint m_last_pos;
};

extern "C" HANDLE DCPCALL ListLoad(HANDLE ParentWin, char* FileToLoad, int ShowFlags) {
    SvgWidget* widget = new SvgWidget(QString::fromUtf8(FileToLoad), (QWidget*)ParentWin);
    widget->show();
    return (HANDLE)widget;
}

extern "C" void DCPCALL ListCloseWindow(HANDLE ListWin) {
    delete (QWidget*)ListWin;
}

extern "C" int DCPCALL ListSearchText(HWND ListWin, char* SearchString, int SearchParameter) {
    return LISTPLUGIN_ERROR;
}

extern "C" int DCPCALL ListSendCommand(HWND ListWin, int Command, int Parameter) {
    return LISTPLUGIN_ERROR;
}

extern "C" void DCPCALL ListSetDefaultParams(ListDefaultParamStruct* dps) {
}

extern "C" void DCPCALL ListGetDetectString(char* DetectString, int maxlen) {
    strncpy(DetectString, "EXT=\"SVG\"", maxlen);
}

#include "wlx_qt.moc"
