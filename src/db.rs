//! Хранение настроек полей в SQLite: отдельный набор значений на каждый чат.

use rusqlite::{params, Connection};
use std::collections::HashMap;
use std::path::Path;
use std::sync::{Arc, Mutex};

pub type DbHandle = Arc<Db>;

pub struct Db {
    conn: Mutex<Connection>,
}

impl Db {
    pub fn open(path: impl AsRef<Path>) -> Result<Db, rusqlite::Error> {
        let conn = Connection::open(path)?;
        conn.execute(
            "CREATE TABLE IF NOT EXISTS settings (
                chat_id INTEGER NOT NULL,
                field   TEXT    NOT NULL,
                value   TEXT    NOT NULL,
                PRIMARY KEY (chat_id, field)
            )",
            [],
        )?;
        Ok(Self {
            conn: Mutex::new(conn),
        })
    }

    /// Все сохранённые значения чата: field -> value.
    /// Поля без записи отсутствуют в карте (значит — значение по умолчанию).
    pub fn values(&self, chat_id: i64) -> HashMap<String, String> {
        let conn = self.conn.lock().unwrap();
        let mut map = HashMap::new();
        let Ok(mut stmt) = conn.prepare("SELECT field, value FROM settings WHERE chat_id = ?1")
        else {
            return map;
        };
        let Ok(rows) = stmt.query_map(params![chat_id], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        }) else {
            return map;
        };
        for (field, value) in rows.flatten() {
            map.insert(field, value);
        }
        map
    }

    /// Значение для отображения: сохранённое (даже пустое), иначе — по умолчанию.
    pub fn display_value(&self, chat_id: i64, field: &str) -> String {
        let conn = self.conn.lock().unwrap();
        let stored: Option<String> = conn
            .query_row(
                "SELECT value FROM settings WHERE chat_id = ?1 AND field = ?2",
                params![chat_id, field],
                |row| row.get(0),
            )
            .ok();
        stored.unwrap_or_else(|| crate::pdf::default_value(field).to_string())
    }

    /// Создать/обновить значение поля.
    pub fn set(&self, chat_id: i64, field: &str, value: &str) -> Result<(), rusqlite::Error> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "INSERT INTO settings (chat_id, field, value) VALUES (?1, ?2, ?3)
             ON CONFLICT(chat_id, field) DO UPDATE SET value = excluded.value",
            params![chat_id, field, value],
        )?;
        Ok(())
    }

    /// Удалить значение поля (поле снова будет пустым/по умолчанию).
    pub fn clear(&self, chat_id: i64, field: &str) -> Result<(), rusqlite::Error> {
        let conn = self.conn.lock().unwrap();
        conn.execute(
            "DELETE FROM settings WHERE chat_id = ?1 AND field = ?2",
            params![chat_id, field],
        )?;
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Тесты
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    /// Временная база в temp-каталоге (уникальное имя на каждый тест).
    fn temp_db() -> (Db, std::path::PathBuf) {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!("ozon_bot_test_{}_{nanos}.db", std::process::id()));
        let db = Db::open(&path).unwrap();
        (db, path)
    }

    #[test]
    fn set_update_get_and_clear() {
        let (db, path) = temp_db();

        // До сохранения — значения по умолчанию (как в веб-версии).
        assert_eq!(db.display_value(1, "Автомобиль"), "");
        assert_eq!(
            db.display_value(1, "Отпуск разрешил"),
            "ИП Кабак Сергей Анатольевич"
        );

        // Создание и обновление.
        db.set(1, "Автомобиль", "BMW X5 A123BC").unwrap();
        assert_eq!(db.display_value(1, "Автомобиль"), "BMW X5 A123BC");
        db.set(1, "Автомобиль", "Audi Q7 B777BB").unwrap();
        assert_eq!(db.display_value(1, "Автомобиль"), "Audi Q7 B777BB");

        // Явно сохранённая пустая строка — это «очищено», а не дефолт.
        db.set(1, "Отпуск разрешил", "").unwrap();
        assert_eq!(db.display_value(1, "Отпуск разрешил"), "");

        // clear возвращает поле к дефолту.
        db.clear(1, "Автомобиль").unwrap();
        assert_eq!(db.display_value(1, "Автомобиль"), "");
        db.clear(1, "Отпуск разрешил").unwrap();
        assert_eq!(
            db.display_value(1, "Отпуск разрешил"),
            "ИП Кабак Сергей Анатольевич"
        );

        drop(db);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn values_are_isolated_per_chat() {
        let (db, path) = temp_db();

        db.set(1, "Автомобиль", "BMW").unwrap();
        db.set(2, "Водитель", "Пётр Петров").unwrap();

        let chat1 = db.values(1);
        let chat2 = db.values(2);
        assert_eq!(chat1.get("Автомобиль").map(String::as_str), Some("BMW"));
        assert_eq!(chat1.get("Водитель"), None);
        assert_eq!(chat2.get("Водитель").map(String::as_str), Some("Пётр Петров"));
        assert_eq!(chat2.get("Автомобиль"), None);

        drop(db);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn values_map_covers_only_stored_fields() {
        let (db, path) = temp_db();
        db.set(7, "Водитель", "Иванов").unwrap();
        let vals = db.values(7);
        assert_eq!(vals.len(), 1);
        assert_eq!(vals.get("Водитель").map(String::as_str), Some("Иванов"));
        // Остальные поля отсутствуют — pdf возьмёт для них дефолт.
        assert!(db.values(8).is_empty());
        drop(db);
        let _ = std::fs::remove_file(path);
    }
}
