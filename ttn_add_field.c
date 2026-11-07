/*
 * ttn_add_field.c — Добавить интерактивные AcroForm-поля (текстовые) в PDF (ТТН).
 *
 * Реализация на чистом Си (C11) с использованием библиотеки MuPDF (mupdf-c).
 * Логика перенесена из Rust-проекта ozon_bot (src/pdf.rs): открывает существующий
 * PDF, создаёт/дополняет словарь /AcroForm, добавляет текстовые поля-виджета
 * с заданными координатами, именами и значениями, сохраняет результат.
 *
 * Поддержка INI-конфигурации (опционально, через -c/--config):
 *   [fields]
 *   Автомобиль = BMW X5 A123BC
 *   Водитель = Иванов Иван Иванович
 *   Отпуск разрешил =
 *
 * Ключевые функции MuPDF, используемые в коде:
 *   fz_new_context / fz_drop_context      — создать / уничтожить контекст MuPDF.
 *   fz_register_document_handlers         — зарегистрировать форматы документов.
 *   pdf_open_document / pdf_drop_document — открыть / закрыть PDF-документ.
 *   pdf_trailer                           — получить словарь трейлера (/Root и т.д.).
 *   pdf_load_page                         — загрузить страницу по индексу (0-based).
 *   pdf_create_annot_raw                  — создать аннотацию-виджета (/Widget) на странице.
 *   pdf_annot_obj                         — получить базовый pdf_obj аннотации.
 *   pdf_dict_puts_drop                    — установить ключ-значение в словаре.
 *   pdf_dict_put_bool                     — установить boolean в словаре.
 *   pdf_dict_put_int                      — установить целое в словаре.
 *   pdf_array_push_drop                   — добавить объект в массив PDF.
 *   pdf_new_dict / pdf_new_array          — создать словарь / массив PDF-объектов.
 *   pdf_new_name / pdf_new_string         — создать имя / строку PDF.
 *   pdf_new_int / pdf_new_real            — создать целое / вещественное.
 *   pdf_new_rect                          — создать PDF-объект прямоугольника.
 *   pdf_add_object_drop                   — добавить объект в документ (xref).
 *   pdf_drop_widget                       — уменьшить refcount виджета.
 *   fz_drop_page / pdf_drop_obj           — освободить страницу / объект.
 *   pdf_save_document                     — сохранить документ в файл.
 *
 * Сборка (пример, macOS/ARM, пути к PyMuPDF-venv):
 *   MUPDF_INC=$HOME/development/local-llm-pdf-ocr/.venv/lib/python3.12/site-packages/pymupdf/mupdf-devel/include
 *   MUPDF_LIB=$HOME/development/local-llm-pdf-ocr/.venv/lib/python3.12/site-packages/pymupdf
 *   gcc -std=c11 -Wall -Wextra -O2 -I"$MUPDF_INC" \
 *       -o ttn_add_field ttn_add_field.c ttn_config.c -L"$MUPDF_LIB" \
 *       -lmupdf -lm -lz \
 *       -framework CoreGraphics -framework CoreText -framework CoreFoundation
 *   DYLD_LIBRARY_PATH="$MUPDF_LIB" ./ttn_add_field in.pdf out.pdf -c settings.ini
 *
 * CLI:
 *   ttn_add_field <вход.pdf> <выход.pdf> [-c <config.ini>]
 *      Добавить все 5 стандартных полей ТТН. Значения берутся из ini-файла;
 *      отсутствующие ключи получают значения по умолчанию.
 *
 *   ttn_add_field <вход.pdf> <выход.pdf> <x0> <y0> <x1> <y1> <имя_поля> <значение>
 *      Добавить одно текстовое поле с заданными координатами.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mupdf/fitz.h>
#include <mupdf/fitz/document.h>
#include <mupdf/pdf.h>
#include <mupdf/pdf/document.h>
#include <mupdf/pdf/object.h>
#include <mupdf/pdf/xref.h>
#include <mupdf/pdf/page.h>
#include <mupdf/pdf/annot.h>
#include <mupdf/pdf/form.h>

#include "ttn_config.h"
#include "ttn_add_field.h"

/* -------------------------------------------------------------------------- */
/* Константы полей ТТН                                                        */
/* -------------------------------------------------------------------------- */

static const ttn_field_t TTF_FIELDS[] = {
    {"Автомобиль",              65.0f,  160.0f, 200.0f, 10.0f},
    {"Водитель",                55.0f,  187.5f, 200.0f, 10.0f},
    {"Отпуск разрешил",         20.0f,  451.0f, 200.0f, 10.0f},
    {"Сдал грузоотправитель",   20.0f,  473.0f, 200.0f, 10.0f},
    {"Товар к перевозке принял",307.5f, 451.0f, 200.0f, 10.0f},
};
#define NUM_TTF_FIELDS (sizeof(TTF_FIELDS) / sizeof(TTF_FIELDS[0]))

const ttn_field_t *ttn_fields(size_t *count)
{
    if (count)
        *count = NUM_TTF_FIELDS;
    return TTF_FIELDS;
}

static const char *DA_STRING = "/Helvetica 6 Tf 0 0 0 rg";

/* -------------------------------------------------------------------------- */
/* Вспомогательные функции                                                    */
/* -------------------------------------------------------------------------- */

/* Значение по умолчанию для поля (если пользователь не задал своё) */
const char *ttn_default_value(const char *field)
{
    (void)field;
    return "";
}

/*
 * utf8_to_utf16be_bom
 *
 * Конвертирует UTF-8 строку в UTF-16BE с BOM (0xFE 0xFF) в начале.
 * Возвращает malloc-буфер (освобождать free()). NULL при ошибке.
 */
static unsigned char *utf8_to_utf16be_bom(const char *utf8, size_t *out_len)
{
    size_t src_len = strlen(utf8);
    unsigned char *buf = malloc(src_len * 4 + 2);
    if (!buf)
        return NULL;

    size_t i = 0, j = 0;

    buf[j++] = 0xFE; /* BOM */
    buf[j++] = 0xFF;

    while (i < src_len) {
        unsigned char c = (unsigned char)utf8[i];
        uint32_t cp;
        int seq_len;

        if (c < 0x80) { cp = c; seq_len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; seq_len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; seq_len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; seq_len = 4; }
        else { free(buf); return NULL; }

        if (i + seq_len > src_len) { free(buf); return NULL; }

        for (int k = 1; k < seq_len; k++) {
            unsigned char cont = (unsigned char)utf8[i + k];
            if ((cont & 0xC0) != 0x80) { free(buf); return NULL; }
            cp = (cp << 6) | (cont & 0x3F);
        }

        if (cp < 0x10000) {
            buf[j++] = (unsigned char)(cp >> 8);
            buf[j++] = (unsigned char)(cp & 0xFF);
        } else {
            uint32_t v = cp - 0x10000;
            uint16_t hi = 0xD800 + (uint16_t)(v >> 10);
            uint16_t lo = 0xDC00 + (uint16_t)(v & 0x3FF);
            buf[j++] = (unsigned char)(hi >> 8);
            buf[j++] = (unsigned char)(hi & 0xFF);
            buf[j++] = (unsigned char)(lo >> 8);
            buf[j++] = (unsigned char)(lo & 0xFF);
        }

        i += seq_len;
    }

    *out_len = j;
    return buf;
}

/*
 * get_or_create_acroform
 *
 * Возвращает pdf_obj на словарь /AcroForm (с refcount +1).
 * Если AcroForm отсутствует — создаёт новый с пустым /Fields и /NeedAppearances=true.
 * Вызывающий обязан вызвать pdf_drop_obj, когда объект больше не нужен.
 */
static pdf_obj *get_or_create_acroform(fz_context *ctx, pdf_document *doc)
{
    pdf_obj *trailer = NULL;
    pdf_obj *root = NULL;
    pdf_obj *existing = NULL;
    pdf_obj *acroform = NULL;
    pdf_obj *fields_arr = NULL;

    fz_var(acroform);
    fz_var(fields_arr);

    fz_try(ctx) {
        trailer = pdf_trailer(ctx, doc);
        if (!trailer)
            fz_throw(ctx, FZ_ERROR_GENERIC, "PDF trailer is missing");

        root = pdf_dict_gets(ctx, trailer, "Root");
        if (!root)
            fz_throw(ctx, FZ_ERROR_GENERIC, "PDF root (/Catalog) is missing");

        existing = pdf_dict_gets(ctx, root, "AcroForm");
        if (existing) {
            return pdf_keep_obj(ctx, existing);
        }

        acroform = pdf_new_dict(ctx, doc, 4);

        fields_arr = pdf_new_array(ctx, doc, 0);
        pdf_dict_puts_drop(ctx, acroform, "Fields", fields_arr);
        fields_arr = NULL;

        pdf_dict_put_bool(ctx, acroform, pdf_new_name(ctx, "NeedAppearances"), 1);

        acroform = pdf_add_object_drop(ctx, doc, acroform);

        pdf_dict_puts_drop(ctx, root, "AcroForm", pdf_keep_obj(ctx, acroform));
    }
    fz_catch(ctx) {
        if (fields_arr)   pdf_drop_obj(ctx, fields_arr);
        if (acroform)     pdf_drop_obj(ctx, acroform);
        fprintf(stderr, "MuPDF error in get_or_create_acroform: %s\n",
                fz_caught_message(ctx));
        fz_rethrow(ctx);
    }

    return acroform;
}

/*
 * get_page_height
 *
 * Возвращает высоту страницы в точках (из /MediaBox). При ошибке — 842 (A4).
 */
static float get_page_height(fz_context *ctx, pdf_page *page)
{
    pdf_obj *page_dict = page->obj;
    pdf_obj *mediabox;
    float height = 842.0f;

    if (!page_dict)
        return height;

    mediabox = pdf_dict_gets(ctx, page_dict, "MediaBox");
    if (!mediabox)
        return height;

    pdf_obj *ury_obj = pdf_array_get(ctx, mediabox, 3);
    if (!ury_obj)
        return height;

    height = pdf_to_real(ctx, ury_obj);
    return height;
}

/*
 * resolve_value
 *
 * Определяет итоговое значение поля:
 * 1. Если в cfg есть ключ — возвращаем его значение (даже пустую строку).
 * 2. Иначе — default_value(field).
 */
static const char *resolve_value(const ttn_config_t *cfg, const char *field)
{
    if (cfg) {
        const char *v = config_get_value(cfg, field);
        if (v != NULL)
            return v;
    }
    return ttn_default_value(field);
}

/*
 * add_text_field
 *
 * Добавляет одно текстовое AcroForm-поле на страницу.
 */
static void add_text_field(fz_context *ctx, pdf_document *doc,
                           pdf_page *page, float page_height,
                           const char *name, const char *value,
                           float x0, float y0, float x1, float y1)
{
    pdf_obj *acroform = NULL;
    pdf_obj *fields_arr = NULL;
    pdf_obj *page_dict = page->obj;
    pdf_obj *page_annots = NULL;
    pdf_annot *widget = NULL;
    pdf_obj *annot_dict = NULL;
    pdf_obj *v_obj = NULL;
    unsigned char *utf16_buf = NULL;
    size_t utf16_len = 0;

    float llx = x0;
    float lly = page_height - y0;
    float urx = x1;
    float ury = page_height - y1;

    fz_var(acroform);
    fz_var(fields_arr);
    fz_var(page_annots);
    fz_var(widget);
    fz_var(v_obj);
    fz_var(utf16_buf);

    fz_try(ctx) {
        acroform = get_or_create_acroform(ctx, doc);

        fields_arr = pdf_dict_gets(ctx, acroform, "Fields");
        if (!fields_arr) {
            fields_arr = pdf_new_array(ctx, doc, 0);
            pdf_dict_puts_drop(ctx, acroform, "Fields", fields_arr);
        }

        page_annots = pdf_dict_gets(ctx, page_dict, "Annots");
        if (!page_annots) {
            page_annots = pdf_new_array(ctx, doc, 0);
            pdf_dict_puts_drop(ctx, page_dict, "Annots", page_annots);
        }

        if (value && value[0] != '\0') {
            utf16_buf = utf8_to_utf16be_bom(value, &utf16_len);
            if (!utf16_buf) {
                fprintf(stderr, "ошибка кодирования значения поля '%s'\n", name);
            } else {
                v_obj = pdf_new_string(ctx, (const char *)utf16_buf, utf16_len);
                free(utf16_buf);
                utf16_buf = NULL;
            }
        }

        widget = pdf_create_annot_raw(ctx, page, PDF_ANNOT_WIDGET);
        if (!widget) {
            fprintf(stderr, "не удалось создать виджет на странице\n");
            if (v_obj) pdf_drop_obj(ctx, v_obj);
            fz_throw(ctx, FZ_ERROR_GENERIC, "cannot create widget");
        }

        annot_dict = pdf_annot_obj(ctx, widget);

        pdf_dict_puts_drop(ctx, annot_dict, "FT",   pdf_new_name(ctx, "Tx"));
        pdf_dict_puts_drop(ctx, annot_dict, "T",    pdf_new_string(ctx, name, strlen(name)));
        if (v_obj) {
            pdf_dict_puts_drop(ctx, annot_dict, "V", v_obj);
            v_obj = NULL;
        }
        pdf_dict_put_int(ctx, annot_dict, pdf_new_name(ctx, "F"), 4);
        pdf_dict_put_int(ctx, annot_dict, pdf_new_name(ctx, "Q"), 0);
        pdf_dict_put_int(ctx, annot_dict, pdf_new_name(ctx, "MaxLen"), 40);
        pdf_dict_puts_drop(ctx, annot_dict, "DA", pdf_new_string(ctx, DA_STRING, strlen(DA_STRING)));

        {
            fz_rect r = fz_make_rect(llx, lly, urx, ury);
            pdf_obj *rect_obj = pdf_new_rect(ctx, doc, r);
            pdf_dict_puts_drop(ctx, annot_dict, "Rect", rect_obj);
        }

        pdf_array_push_drop(ctx, fields_arr, pdf_keep_obj(ctx, annot_dict));

        pdf_dict_put_bool(ctx, acroform, pdf_new_name(ctx, "NeedAppearances"), 1);

        pdf_drop_obj(ctx, acroform);
        pdf_drop_widget(ctx, widget);
        widget = NULL;
        annot_dict = NULL;
    }
    fz_catch(ctx) {
        fprintf(stderr, "MuPDF error adding field '%s': %s\n", name,
                fz_caught_message(ctx));
        if (utf16_buf)   free(utf16_buf);
        if (v_obj)       pdf_drop_obj(ctx, v_obj);
        if (widget)      pdf_drop_widget(ctx, widget);
        if (acroform)    pdf_drop_obj(ctx, acroform);
        fz_rethrow(ctx);
    }
}

/*
 * add_ttn_fields — добавить все 5 стандартных полей ТТН на страницу.
 */
static void add_ttn_fields(fz_context *ctx, pdf_document *doc,
                           pdf_page *page, float page_height,
                           const ttn_config_t *cfg)
{
    for (size_t i = 0; i < NUM_TTF_FIELDS; i++) {
        const ttn_field_t *f = &TTF_FIELDS[i];
        const char *value = resolve_value(cfg, f->name);

        add_text_field(ctx, doc, page, page_height,
                       f->name, value,
                       f->x, f->y, f->x + f->w, f->y + f->h);
    }
}

/* -------------------------------------------------------------------------- */
/* Основные функции обработки                                                 */
/* -------------------------------------------------------------------------- */

int process_pdf_file(const char *in_path, const char *out_path,
                     const ttn_config_t *cfg)
{
    fz_context *ctx = NULL;
    pdf_document *doc = NULL;
    pdf_page *page = NULL;
    float page_height = 0.0f;
    int result = EXIT_FAILURE;

    fz_var(doc);
    fz_var(page);
    fz_var(page_height);
    fz_var(result);

    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) {
        fprintf(stderr, "ttn: cannot create fz_context\n");
        return EXIT_FAILURE;
    }

    fz_register_document_handlers(ctx);

    fz_try(ctx) {
        doc = pdf_open_document(ctx, in_path);
        page = pdf_load_page(ctx, doc, 0);
        if (!page)
            fz_throw(ctx, FZ_ERROR_GENERIC, "cannot load page 0");

        page_height = get_page_height(ctx, page);

        add_ttn_fields(ctx, doc, page, page_height, cfg);

        pdf_save_document(ctx, doc, out_path, NULL);

        result = EXIT_SUCCESS;
    }
    fz_catch(ctx) {
        fprintf(stderr, "MuPDF error: %s\n", fz_caught_message(ctx));
        result = EXIT_FAILURE;
    }

    if (page)     fz_drop_page(ctx, (fz_page *)page);
    if (doc)      pdf_drop_document(ctx, doc);
    if (ctx)      fz_drop_context(ctx);

    return result;
}

int process_pdf_file_single(const char *in_path, const char *out_path,
                            const char *field_name, const char *field_value,
                            float x0, float y0, float x1, float y1)
{
    fz_context *ctx = NULL;
    pdf_document *doc = NULL;
    pdf_page *page = NULL;
    float page_height = 0.0f;
    int result = EXIT_FAILURE;

    fz_var(doc);
    fz_var(page);
    fz_var(page_height);
    fz_var(result);

    ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) {
        fprintf(stderr, "ttn: cannot create fz_context\n");
        return EXIT_FAILURE;
    }

    fz_register_document_handlers(ctx);

    fz_try(ctx) {
        doc = pdf_open_document(ctx, in_path);
        page = pdf_load_page(ctx, doc, 0);
        if (!page) fz_throw(ctx, FZ_ERROR_GENERIC, "cannot load page 0");

        page_height = get_page_height(ctx, page);

        add_text_field(ctx, doc, page, page_height,
                       field_name, field_value,
                       x0, y0, x1, y1);

        pdf_save_document(ctx, doc, out_path, NULL);
        result = EXIT_SUCCESS;
    }
    fz_catch(ctx) {
        fprintf(stderr, "MuPDF error: %s\n", fz_caught_message(ctx));
    }

    if (page)     fz_drop_page(ctx, (fz_page *)page);
    if (doc)      pdf_drop_document(ctx, doc);
    if (ctx)      fz_drop_context(ctx);

    return result;
}

#ifndef TTN_NO_MAIN
/* -------------------------------------------------------------------------- */
/* CLI                                                                        */
/* -------------------------------------------------------------------------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Использование:\n"
            "  %s <вход.pdf> <выход.pdf> [-c <config.ini>]\n"
            "      Добавить все 5 стандартных полей ТТН. Значения берутся из ini-файла\n"
            "      (секция [fields]); отсутствующие ключи получают значения по умолчанию.\n"
            "\n"
            "  %s <вход.pdf> <выход.pdf> <x0> <y0> <x1> <y1> <имя_поля> <значение>\n"
            "      Добавить одно текстовое поле с заданными координатами.\n"
            "\n"
            "Формат config.ini:\n"
            "  [fields]\n"
            "  Автомобиль = BMW X5 A123BC\n"
            "  Водитель = Иванов Иван Иванович\n"
            "  Отпуск разрешил =\n"
            "\n"
            "Пример:\n"
            "  %s ttn.pdf out.pdf -c settings.ini\n"
            "  %s ttn.pdf out.pdf 65 170 265 180 \"Автомобиль\" \"BMW X5\"\n",
            prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *out_path = NULL;
    const char *ini_path = NULL;
    int argi = 1;

    while (argi < argc) {
        if (strcmp(argv[argi], "-c") == 0 || strcmp(argv[argi], "--config") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "ошибка: -c требует путь к ini-файлу\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            ini_path = argv[argi + 1];
            argi += 2;
        } else if (!in_path) {
            in_path = argv[argi];
            argi++;
        } else if (!out_path) {
            out_path = argv[argi];
            argi++;
        } else {
            break;
        }
    }

    if (!in_path || !out_path) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int remaining = argc - argi;

    if (remaining == 6) {
        float x0 = strtof(argv[argi], NULL);
        float y0 = strtof(argv[argi + 1], NULL);
        float x1 = strtof(argv[argi + 2], NULL);
        float y1 = strtof(argv[argi + 3], NULL);
        const char *field_name = argv[argi + 4];
        const char *field_value = argv[argi + 5];

        printf("Обработка: %s -> %s (поле '%s'=%s, rect=%.2f,%.2f,%.2f,%.2f)\n",
               in_path, out_path, field_name, field_value, x0, y0, x1, y1);

        return process_pdf_file_single(in_path, out_path,
                                       field_name, field_value,
                                       x0, y0, x1, y1);
    }

    if (remaining != 0) {
        fprintf(stderr, "ошибка: неожиданные аргументы после путей к файлам\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    ttn_config_t cfg;
    int cfg_ok = 0;

    if (ini_path) {
        if (parse_ini(ini_path, &cfg) == 0) {
            cfg_ok = 1;
            printf("Загружена конфигурация: %s (%d полей)\n", ini_path, cfg.count);
        } else {
            fprintf(stderr, "предупреждение: не удалось прочитать ini-файл '%s', "
                    "будут использованы значения по умолчанию\n", ini_path);
        }
    }

    printf("Обработка: %s -> %s (5 полей ТТН)\n", in_path, out_path);

    return process_pdf_file(in_path, out_path,
                            cfg_ok ? &cfg : NULL);
}

#endif /* TTN_NO_MAIN */
