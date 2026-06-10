/****************************************************************************
** Meta object code from reading C++ file 'FilterableHeaderView.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../include/wayland_qt_base/FilterableHeaderView.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'FilterableHeaderView.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN10QtWlPlugin20FilterableHeaderViewE_t {};
} // unnamed namespace

template <> constexpr inline auto QtWlPlugin::FilterableHeaderView::qt_create_metaobjectdata<qt_meta_tag_ZN10QtWlPlugin20FilterableHeaderViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtWlPlugin::FilterableHeaderView",
        "filterChanged",
        "",
        "column",
        "text",
        "adjustInputPositions"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'filterChanged'
        QtMocHelpers::SignalData<void(int, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { QMetaType::QString, 4 },
        }}),
        // Slot 'adjustInputPositions'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<FilterableHeaderView, qt_meta_tag_ZN10QtWlPlugin20FilterableHeaderViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QtWlPlugin::FilterableHeaderView::staticMetaObject = { {
    QMetaObject::SuperData::link<QHeaderView::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10QtWlPlugin20FilterableHeaderViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10QtWlPlugin20FilterableHeaderViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN10QtWlPlugin20FilterableHeaderViewE_t>.metaTypes,
    nullptr
} };

void QtWlPlugin::FilterableHeaderView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<FilterableHeaderView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->filterChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 1: _t->adjustInputPositions(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (FilterableHeaderView::*)(int , const QString & )>(_a, &FilterableHeaderView::filterChanged, 0))
            return;
    }
}

const QMetaObject *QtWlPlugin::FilterableHeaderView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QtWlPlugin::FilterableHeaderView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10QtWlPlugin20FilterableHeaderViewE_t>.strings))
        return static_cast<void*>(this);
    return QHeaderView::qt_metacast(_clname);
}

int QtWlPlugin::FilterableHeaderView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QHeaderView::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 2;
    }
    return _id;
}

// SIGNAL 0
void QtWlPlugin::FilterableHeaderView::filterChanged(int _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}
QT_WARNING_POP
