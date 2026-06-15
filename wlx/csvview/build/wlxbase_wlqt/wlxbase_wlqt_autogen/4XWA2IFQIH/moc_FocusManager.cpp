/****************************************************************************
** Meta object code from reading C++ file 'FocusManager.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../wlxbase_wlqt/include/wlxbase_wlqt/FocusManager.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'FocusManager.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN10QtWlPlugin12FocusManagerE_t {};
} // unnamed namespace

template <> constexpr inline auto QtWlPlugin::FocusManager::qt_create_metaobjectdata<qt_meta_tag_ZN10QtWlPlugin12FocusManagerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QtWlPlugin::FocusManager",
        "activated",
        "",
        "deactivated",
        "inputWidgetEntered",
        "QWidget*",
        "w",
        "inputWidgetExited"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'activated'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'deactivated'
        QtMocHelpers::SignalData<void()>(3, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'inputWidgetEntered'
        QtMocHelpers::SignalData<void(QWidget *)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Signal 'inputWidgetExited'
        QtMocHelpers::SignalData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<FocusManager, qt_meta_tag_ZN10QtWlPlugin12FocusManagerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QtWlPlugin::FocusManager::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10QtWlPlugin12FocusManagerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10QtWlPlugin12FocusManagerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN10QtWlPlugin12FocusManagerE_t>.metaTypes,
    nullptr
} };

void QtWlPlugin::FocusManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<FocusManager *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->activated(); break;
        case 1: _t->deactivated(); break;
        case 2: _t->inputWidgetEntered((*reinterpret_cast<std::add_pointer_t<QWidget*>>(_a[1]))); break;
        case 3: _t->inputWidgetExited(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 2:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< QWidget* >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (FocusManager::*)()>(_a, &FocusManager::activated, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (FocusManager::*)()>(_a, &FocusManager::deactivated, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (FocusManager::*)(QWidget * )>(_a, &FocusManager::inputWidgetEntered, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (FocusManager::*)()>(_a, &FocusManager::inputWidgetExited, 3))
            return;
    }
}

const QMetaObject *QtWlPlugin::FocusManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QtWlPlugin::FocusManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10QtWlPlugin12FocusManagerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int QtWlPlugin::FocusManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void QtWlPlugin::FocusManager::activated()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void QtWlPlugin::FocusManager::deactivated()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void QtWlPlugin::FocusManager::inputWidgetEntered(QWidget * _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void QtWlPlugin::FocusManager::inputWidgetExited()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}
QT_WARNING_POP
