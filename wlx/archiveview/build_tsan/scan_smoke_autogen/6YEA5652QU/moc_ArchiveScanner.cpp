/****************************************************************************
** Meta object code from reading C++ file 'ArchiveScanner.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../include/ArchiveScanner.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ArchiveScanner.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN14ArchiveScannerE_t {};
} // unnamed namespace

template <> constexpr inline auto ArchiveScanner::qt_create_metaobjectdata<qt_meta_tag_ZN14ArchiveScannerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "ArchiveScanner",
        "formatDetected",
        "",
        "format",
        "filters",
        "commentFound",
        "comment",
        "entriesReady",
        "archiveview::EntryBatch",
        "batch",
        "progress",
        "bytesRead",
        "totalBytes",
        "scanFinished",
        "ok",
        "error",
        "archiveview::Summary",
        "summary",
        "passphraseRequested",
        "attempt"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'formatDetected'
        QtMocHelpers::SignalData<void(const QString &, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 }, { QMetaType::QString, 4 },
        }}),
        // Signal 'commentFound'
        QtMocHelpers::SignalData<void(const QString &)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 6 },
        }}),
        // Signal 'entriesReady'
        QtMocHelpers::SignalData<void(const archiveview::EntryBatch &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 8, 9 },
        }}),
        // Signal 'progress'
        QtMocHelpers::SignalData<void(qint64, qint64)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::LongLong, 11 }, { QMetaType::LongLong, 12 },
        }}),
        // Signal 'scanFinished'
        QtMocHelpers::SignalData<void(bool, const QString &, const archiveview::Summary &)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 14 }, { QMetaType::QString, 15 }, { 0x80000000 | 16, 17 },
        }}),
        // Signal 'passphraseRequested'
        QtMocHelpers::SignalData<void(int)>(18, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 19 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<ArchiveScanner, qt_meta_tag_ZN14ArchiveScannerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject ArchiveScanner::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14ArchiveScannerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14ArchiveScannerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14ArchiveScannerE_t>.metaTypes,
    nullptr
} };

void ArchiveScanner::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<ArchiveScanner *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->formatDetected((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 1: _t->commentFound((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->entriesReady((*reinterpret_cast<std::add_pointer_t<archiveview::EntryBatch>>(_a[1]))); break;
        case 3: _t->progress((*reinterpret_cast<std::add_pointer_t<qint64>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<qint64>>(_a[2]))); break;
        case 4: _t->scanFinished((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<archiveview::Summary>>(_a[3]))); break;
        case 5: _t->passphraseRequested((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
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
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< archiveview::EntryBatch >(); break;
            }
            break;
        case 4:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 2:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< archiveview::Summary >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (ArchiveScanner::*)(const QString & , const QString & )>(_a, &ArchiveScanner::formatDetected, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (ArchiveScanner::*)(const QString & )>(_a, &ArchiveScanner::commentFound, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (ArchiveScanner::*)(const archiveview::EntryBatch & )>(_a, &ArchiveScanner::entriesReady, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (ArchiveScanner::*)(qint64 , qint64 )>(_a, &ArchiveScanner::progress, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (ArchiveScanner::*)(bool , const QString & , const archiveview::Summary & )>(_a, &ArchiveScanner::scanFinished, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (ArchiveScanner::*)(int )>(_a, &ArchiveScanner::passphraseRequested, 5))
            return;
    }
}

const QMetaObject *ArchiveScanner::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ArchiveScanner::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14ArchiveScannerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ArchiveScanner::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void ArchiveScanner::formatDetected(const QString & _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}

// SIGNAL 1
void ArchiveScanner::commentFound(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void ArchiveScanner::entriesReady(const archiveview::EntryBatch & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void ArchiveScanner::progress(qint64 _t1, qint64 _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2);
}

// SIGNAL 4
void ArchiveScanner::scanFinished(bool _t1, const QString & _t2, const archiveview::Summary & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1, _t2, _t3);
}

// SIGNAL 5
void ArchiveScanner::passphraseRequested(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}
QT_WARNING_POP
