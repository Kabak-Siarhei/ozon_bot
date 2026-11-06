//! Генерирует ini-файл из настроек пользователя и вызывает C-утилиту ttn_add_field.
//! Нет зависимостей от Python — только MuPDF + стандартная библиотека Си.

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{Context, Result};

pub const TTN_FIELDS: [&str; 5] = [
    "Автомобиль",
    "Водитель",
    "Отпуск разрешил",
    "Сдал грузоотправитель",
    "Товар к перевозке принял",
];

fn ttn_binary_path() -> PathBuf {
    env::var("TTN_BIN_PATH")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("./ttn_add_field"))
}

/// Пишет ini-файл из настроек конкретного пользователя.
pub fn write_settings_ini(
    values: &HashMap<String, String>,
    ini_path: &Path,
) -> Result<()> {
    let mut content = String::from("[fields]\n");

    for field in TTN_FIELDS {
        let value = values.get(field).cloned().unwrap_or_default();
        let escaped = value.replace('"', "\\\"");
        content.push_str(&format!("{} = \"{}\"\n", field, escaped));
    }

    fs::write(ini_path, content)
        .with_context(|| format!("не удалось записать {:?}", ini_path))?;

    Ok(())
}

/// Запускает ttn_add_field с ini-файлом пользователя.
pub fn process_pdf(
    input_path: &Path,
    output_path: &Path,
    ini_path: &Path,
) -> Result<()> {
    let bin = ttn_binary_path();

    if !bin.exists() {
        anyhow::bail!(
            "бинарник ttn_add_field не найден по {:?}. Установите TTN_BIN_PATH.",
            bin
        );
    }

    let status = Command::new(&bin)
        .arg(input_path)
        .arg(output_path)
        .arg("-c")
        .arg(ini_path)
        .status()
        .with_context(|| format!("не удалось запустить {:?}", bin))?;

    if !status.success() {
        anyhow::bail!("ttn_add_field завершился с ошибкой");
    }

    Ok(())
}

/// Полный цикл: настройки из SQLite → ini → вызов C-утилиты.
pub fn process_pdf_with_settings(
    chat_id: i64,
    db: &crate::db::Db,
    input_path: &Path,
    output_path: &Path,
    tmp_dir: &Path,
) -> Result<()> {
    let values = db.values(chat_id);

    let ini_path = tmp_dir.join(format!("ttn_settings_{chat_id}.ini"));
    write_settings_ini(&values, &ini_path)?;

    let result = process_pdf(input_path, output_path, &ini_path);

    let _ = fs::remove_file(&ini_path);

    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_ini_with_all_fields_and_empty_values() {
        let dir = std::env::temp_dir();
        let path = dir.join(format!("ttn_ini_test_{}.ini", std::process::id()));

        let mut values = HashMap::new();
        values.insert("Автомобиль".to_string(), "BMW X5 A123BC".to_string());
        values.insert("Отпуск разрешил".to_string(), String::new());

        write_settings_ini(&values, &path).unwrap();
        let content = std::fs::read_to_string(&path).unwrap();
        let _ = std::fs::remove_file(&path);

        assert!(content.starts_with("[fields]\n"));
        assert!(content.contains("Автомобиль = \"BMW X5 A123BC\""));
        assert!(content.contains("Отпуск разрешил = \"\""));
        // Поля без значения из карты пишутся пустыми.
        assert!(content.contains("Водитель = \"\""));
    }
}
