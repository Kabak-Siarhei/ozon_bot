//! Обработка PDF: добавление AcroForm-полей.
//! Логика перенесена из веб-сервера `ozon`, значения полей теперь
//! берутся из настроек бота (HashMap), а не зашиты в код.

use lopdf::{dictionary, Document, Object, ObjectId, StringFormat};
use phf::phf_map;
use std::collections::HashMap;
use std::error::Error;

/// Строка внешнего вида с нужным шрифтом и размером.
const DA_STRING: &str = "/Helvetica 6 Tf 0 0 0 rg";

/// Ключевые слова (названия полей) — порядок отображения в настройках.
pub const FIELDS: [&str; 5] = [
    "Автомобиль",
    "Водитель",
    "Отпуск разрешил",
    "Сдал грузоотправитель",
    "Товар к перевозке принял",
];

/// Позиции и размеры полей на странице: (x, y, ширина, высота).
static KEYWORD_PARAMS: phf::Map<&'static str, (f64, f64, f64, f64)> = phf_map! {
    "Автомобиль" => (65.0, 170.0, 200.0, 10.0),
    "Водитель" => (55.0, 197.5, 200.0, 10.0),
    "Отпуск разрешил" => (20.0, 461.0, 200.0, 10.0),
    "Сдал грузоотправитель" => (20.0, 483.0, 200.0, 10.0),
    "Товар к перевозке принял" => (307.5, 461.0, 200.0, 10.0),
};

/// Значения по умолчанию (как в веб-версии), пока пользователь не задал свои.
pub fn default_value(field: &str) -> &'static str {
    match field {
        "Отпуск разрешил" | "Сдал грузоотправитель" => "ИП Кабак Сергей Анатольевич",
        _ => "",
    }
}

/// Обрабатывает PDF из байтов и возвращает новые байты с заполненными полями.
///
/// `values` — значения из настроек бота (field -> value).
/// Поле, отсутствующее в карте, получает [`default_value`].
pub fn process_pdf(
    input: &[u8],
    values: &HashMap<String, String>,
) -> Result<Vec<u8>, Box<dyn Error + Send + Sync>> {
    let mut doc = Document::load_mem(input)?;
    let page_id = doc
        .get_pages()
        .get(&1)
        .ok_or("Страница не найдена")?
        .clone();
    add_acroform_fields(&mut doc, page_id, values)?;
    doc.compress();
    let mut out = Vec::new();
    doc.save_to(&mut out)?;
    Ok(out)
}

/// Возвращает ширину и высоту страницы в пунктах (points) по `/MediaBox`.
/// При отсутствии/ошибке возвращает `None` (вызовет значение по умолчанию).
fn get_page_size(doc: &Document, page_id: ObjectId) -> Option<(f64, f64)> {
    let obj = doc.get_object(page_id).ok()?;
    let dict = obj.as_dict().ok()?;
    let media_box = dict.get(b"MediaBox").ok()?.as_array().ok()?;
    if media_box.len() < 4 {
        return None;
    }
    let num = |o: &Object| -> Option<f64> {
        match o {
            Object::Integer(i) => Some(*i as f64),
            Object::Real(f) => Some(*f as f64),
            _ => None,
        }
    };
    let llx = num(&media_box[0])?;
    let lly = num(&media_box[1])?;
    let urx = num(&media_box[2])?;
    let ury = num(&media_box[3])?;
    Some((urx - llx, ury - lly))
}

fn add_acroform_fields(
    doc: &mut Document,
    page_id: ObjectId,
    values: &HashMap<String, String>,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    let acroform_id = get_or_create_acroform(doc)?;
    // Значение по умолчанию — формат A4 (если MediaBox не прочитался).
    let (_, height) = get_page_size(doc, page_id).unwrap_or((595.0, 842.0));

    // --- Чтение существующих массивов ---
    let page_annots = doc
        .get_object(page_id)
        .ok()
        .and_then(|obj| obj.as_dict().ok())
        .and_then(|dict| dict.get(b"Annots").ok())
        .and_then(|obj| obj.as_array().ok())
        .map(|arr| arr.clone())
        .unwrap_or_else(Vec::new);

    let fields_array = doc
        .get_object(acroform_id)
        .ok()
        .and_then(|obj| obj.as_dict().ok())
        .and_then(|dict| dict.get(b"Fields").ok())
        .and_then(|obj| obj.as_array().ok())
        .map(|arr| arr.clone())
        .unwrap_or_else(Vec::new);

    // --- Создание полей ---
    let mut new_field_ids = Vec::new();

    for (field_name, &(x, y, w, h)) in KEYWORD_PARAMS.entries() {
        // Значение из настроек, иначе — значение по умолчанию.
        let value: &str = match values.get(*field_name) {
            Some(v) => v.as_str(),
            None => default_value(field_name),
        };

        let llx: f64 = x;
        let lly: f64 = height - y;
        let urx: f64 = llx + w;
        let ury: f64 = lly + h;

        let v_obj = if value.is_empty() {
            Object::Null
        } else {
            let utf16_bytes: Vec<u8> = value
                .encode_utf16()
                .flat_map(|c| c.to_be_bytes())
                .collect();
            let mut with_bom = vec![0xFE, 0xFF]; // BOM
            with_bom.extend(utf16_bytes);
            Object::String(with_bom, StringFormat::Hexadecimal)
        };

        let annot_dict = dictionary! {
            "Type" => "Annot",
            "Subtype" => "Widget",
            "FT" => "Tx",
            "T" => Object::string_literal(*field_name),
            "Rect" => Object::Array(vec![llx.into(), lly.into(), urx.into(), ury.into()]),
            "V" => v_obj,
            "P" => page_id,
            "F" => Object::Integer(4),
            "Q" => Object::Integer(0),            // 0 - лево, 1 - центр, 2 - право
            "MaxLen" => Object::Integer(40),       // максимальная длина ввода
            "DA" => Object::string_literal(DA_STRING), // шрифт и размер
        };
        let annot_id = doc.add_object(Object::Dictionary(annot_dict));
        new_field_ids.push(annot_id);
    }

    // --- Обновление массивов ---
    let mut page_annots = page_annots;
    let mut fields_array = fields_array;
    for id in &new_field_ids {
        page_annots.push((*id).into());
        fields_array.push((*id).into());
    }

    let page_dict = doc.get_object_mut(page_id)?.as_dict_mut()?;
    page_dict.set("Annots", Object::Array(page_annots));

    let acroform_dict = doc.get_object_mut(acroform_id)?.as_dict_mut()?;
    acroform_dict.set("Fields", Object::Array(fields_array));
    acroform_dict.set("NeedAppearances", Object::Boolean(true));

    Ok(())
}

/// Ищет или создаёт словарь AcroForm в корне документа.
fn get_or_create_acroform(
    doc: &mut Document,
) -> Result<ObjectId, Box<dyn Error + Send + Sync>> {
    let catalog_id = doc.trailer.get(b"Root")?.as_reference()?;

    // 1. Проверяем, есть ли уже AcroForm.
    let existing_acroform = {
        let catalog = doc.get_object(catalog_id)?.as_dict()?;
        catalog
            .get(b"AcroForm")
            .ok()
            .map(|v| v.as_reference().unwrap())
    };

    if let Some(id) = existing_acroform {
        return Ok(id);
    }

    // 2. Создаём новый объект AcroForm.
    let acroform_dict = dictionary! {
        "Fields" => Object::Array(vec![]),
        "NeedAppearances" => Object::Boolean(true),
    };
    let acroform_id = doc.add_object(Object::Dictionary(acroform_dict));

    // 3. Устанавливаем AcroForm в каталог.
    let catalog = doc.get_object_mut(catalog_id)?.as_dict_mut()?;
    catalog.set("AcroForm", acroform_id);

    Ok(acroform_id)
}

// ---------------------------------------------------------------------------
// Тесты
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    /// Реальный бланк ТТН из веб-проекта (tests/fixtures/ttn.pdf).
    fn fixture_bytes() -> Vec<u8> {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/ttn.pdf");
        std::fs::read(path).expect("фикстура ttn.pdf не найдена")
    }

    /// Декодирует значение поля: UTF-16BE с BOM (как пишет add_acroform_fields).
    fn decode_utf16(bytes: &[u8]) -> String {
        let body = if bytes.len() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF {
            &bytes[2..]
        } else {
            bytes
        };
        let units: Vec<u16> = body
            .chunks_exact(2)
            .map(|c| u16::from_be_bytes([c[0], c[1]]))
            .collect();
        String::from_utf16(&units).expect("некорректный UTF-16")
    }

    /// Читает из документа все поля AcroForm: (название, значение).
    /// `None` — поля нет или оно пустое (Object::Null).
    fn acroform_fields(doc: &Document) -> Vec<(String, Option<String>)> {
        let root = doc.trailer.get(b"Root").unwrap().as_reference().unwrap();
        let catalog = doc.get_object(root).unwrap().as_dict().unwrap();
        let acro_id = catalog.get(b"AcroForm").unwrap().as_reference().unwrap();
        let acro = doc.get_object(acro_id).unwrap().as_dict().unwrap();
        let refs = acro.get(b"Fields").unwrap().as_array().unwrap().clone();

        let mut out = Vec::new();
        for r in &refs {
            let id = r.as_reference().expect("поле должно быть ссылкой");
            let dict = doc.get_object(id).unwrap().as_dict().unwrap();
            let name = match dict.get(b"T").unwrap() {
                Object::String(s, _) => String::from_utf8(s.clone()).unwrap(),
                other => panic!("неожиданное T: {other:?}"),
            };
            let value = match dict.get(b"V") {
                Ok(Object::String(s, _)) => Some(decode_utf16(s)),
                _ => None,
            };
            out.push((name, value));
        }
        out
    }

    fn value<'a>(fields: &'a [(String, Option<String>)], name: &str) -> Option<&'a str> {
        fields
            .iter()
            .find(|(n, _)| n == name)
            .map(|(_, v)| v.as_deref())
            .unwrap_or(None)
    }

    #[test]
    fn adds_all_five_fields_with_default_values() {
        let out = process_pdf(&fixture_bytes(), &HashMap::new()).unwrap();
        let doc = Document::load_mem(&out).unwrap();
        assert!(!doc.get_pages().is_empty(), "результат должен содержать страницу");

        let fields = acroform_fields(&doc);
        for name in FIELDS {
            assert!(
                fields.iter().any(|(n, _)| n == name),
                "поле «{name}» отсутствует: {fields:?}"
            );
        }

        // Дефолты из веб-версии.
        assert_eq!(
            value(&fields, "Отпуск разрешил"),
            Some("ИП Кабак Сергей Анатольевич")
        );
        assert_eq!(
            value(&fields, "Сдал грузоотправитель"),
            Some("ИП Кабак Сергей Анатольевич")
        );
        // У остальных дефолт пустой -> Object::Null.
        assert_eq!(value(&fields, "Автомобиль"), None);
        assert_eq!(value(&fields, "Водитель"), None);
        assert_eq!(value(&fields, "Товар к перевозке принял"), None);
    }

    #[test]
    fn applies_custom_values_from_settings() {
        let mut values = HashMap::new();
        values.insert("Автомобиль".to_string(), "BMW X5 A123BC".to_string());
        values.insert("Водитель".to_string(), "Иванов Иван Иванович".to_string());
        // Очищенное в настройках поле -> Null, а не дефолт.
        values.insert("Отпуск разрешил".to_string(), String::new());

        let out = process_pdf(&fixture_bytes(), &values).unwrap();
        let doc = Document::load_mem(&out).unwrap();
        let fields = acroform_fields(&doc);

        assert_eq!(value(&fields, "Автомобиль"), Some("BMW X5 A123BC"));
        assert_eq!(value(&fields, "Водитель"), Some("Иванов Иван Иванович"));
        assert_eq!(value(&fields, "Отпуск разрешил"), None);
        // Не тронутые в настройках поля берут дефолт.
        assert_eq!(
            value(&fields, "Сдал грузоотправитель"),
            Some("ИП Кабак Сергей Анатольевич")
        );
        assert_eq!(value(&fields, "Товар к перевозке принял"), None);
    }

    #[test]
    fn is_idempotent_on_processed_document() {
        // Повторная обработка уже обработанного файла не должна падать.
        let once = process_pdf(&fixture_bytes(), &HashMap::new()).unwrap();
        let twice = process_pdf(&once, &HashMap::new()).unwrap();
        assert!(!twice.is_empty());
    }

    #[test]
    fn rejects_garbage_input() {
        assert!(process_pdf("это не PDF".as_bytes(), &HashMap::new()).is_err());
    }
}
