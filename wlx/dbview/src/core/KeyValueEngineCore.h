#pragma once

#include "core/DbEngineCore.h"
#include <functional>

/// Shared base for the key/value engines (LMDB, Berkeley DB, optionally
/// RocksDB/LevelDB): a single "<keys>" pseudo-table, 2 columns (Key,
/// Value), full eager load into memory (simpler than the Qt side's
/// windowed KeyValueModel -- a deliberate GTK-port simplification).
class KeyValueEngineCoreBase : public DbEngineCore {
public:
    std::vector<std::string> tableNames() const override { return {"<keys>"}; }
    std::string currentTableName() const override { return "<keys>"; }
    bool supportsMultipleTables() const override { return false; }
    bool supportsSubmitRevert() const override { return true; }
    std::string lastError() const override { return m_lastError; }

    bool selectTable(const std::string &) override {
        m_keys.clear(); m_values.clear(); m_binary.clear();
        if (!fetchAll) return false;
        fetchAll(m_keys, m_values);
        m_binary.resize(m_values.size());
        for (size_t i = 0; i < m_values.size(); i++)
            m_binary[i] = !isValidUtf8(m_values[i]);
        return true;
    }

    int rowCount() const override { return (int)m_keys.size(); }
    int columnCount() const override { return 2; }
    std::string columnName(int col) const override { return col == 0 ? "Key" : "Value"; }

    std::string cellText(int row, int col) const override {
        if (row < 0 || row >= (int)m_keys.size()) return "";
        if (col == 0) return isValidUtf8(m_keys[row]) ? m_keys[row] : toHex(m_keys[row]);
        return m_binary[row] ? ("[Binary Data - " + std::to_string(m_values[row].size()) + " bytes]") : m_values[row];
    }
    bool cellIsBinary(int row, int col) const override { return col == 1 && row >= 0 && row < (int)m_binary.size() && m_binary[row]; }
    bool cellEditable(int, int col) const override { return col == 1 && !m_readOnly; }

    bool setCellText(int row, int col, const std::string &text) override {
        if (col != 1 || row < 0 || row >= (int)m_keys.size() || !putValue) return false;
        if (!putValue(m_keys[row], text)) return false;
        m_values[row] = text;
        m_binary[row] = !isValidUtf8(text);
        return true;
    }

    bool submitAll() override { return true; } // writes are immediate (see setCellText)
    bool revertAll() override { return selectTable(""); }

    std::function<void(std::vector<std::string> &keys, std::vector<std::string> &values)> fetchAll;
    std::function<bool(const std::string &key, const std::string &value)> putValue;

protected:
    static bool isValidUtf8(const std::string &s) {
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = s[i];
            int n;
            if ((c & 0x80) == 0) n = 0;
            else if ((c & 0xE0) == 0xC0) n = 1;
            else if ((c & 0xF0) == 0xE0) n = 2;
            else if ((c & 0xF8) == 0xF0) n = 3;
            else return false;
            if (i + n >= s.size() + 1 && n > 0 && i + n > s.size()) return false;
            for (int k = 1; k <= n; k++) {
                if (i + k >= s.size() || (((unsigned char)s[i + k]) & 0xC0) != 0x80) return false;
            }
            i += n + 1;
        }
        return true;
    }
    static std::string toHex(const std::string &s) {
        static const char *hex = "0123456789abcdef";
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            if (i) out += ' ';
            unsigned char c = s[i];
            out += hex[c >> 4]; out += hex[c & 0xF];
        }
        return out;
    }

    std::vector<std::string> m_keys, m_values;
    std::vector<bool> m_binary;
    std::string m_lastError;
};
