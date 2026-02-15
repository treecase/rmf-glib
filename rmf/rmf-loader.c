#include "rmf/rmf-loader.h"

#include "glib.h"
#include "rmf/rmf-private.h"
#include "rmf/rmf-root.h"

#include <glib-object.h>
#include <stddef.h>

// clang-format off
G_DEFINE_QUARK(rmf-loader-error-quark, rmf_loader_error)
// clang-format on

/**
 * RmfLoaderError:
 * @RMF_LOADER_ERROR_XYZ: xyz
 *
 * Error codes for `RMF_LOADER_ERROR`.
 */
G_DEFINE_ENUM_TYPE(
    RmfLoaderError,
    rmf_loader_error,
    G_DEFINE_ENUM_VALUE(RMF_LOADER_ERROR_XYZ, "xyz")
)

static constexpr rmf_float RMF_MIN_SUPPORTED_VERSION = 1.6f;
static constexpr rmf_float RMF_MAX_SUPPORTED_VERSION = 2.2f;

/**
 * RmfLoader:
 *
 * Loads RMF data from various* sources.
 *
 * (*"Various" presently meaning one).
 */
struct _RmfLoader {
    GObject parent_instance;
    char const *source;
    GBytes *data;
    goffset offset;
    GPtrArray *tag_stack;
    rmf_float version;
    RmfRoot *root;
};

G_DEFINE_FINAL_TYPE(RmfLoader, rmf_loader, G_TYPE_OBJECT)

enum RmfLoaderProperty {
    PROP_SOURCE = 1,
    PROP_DATA,
    PROP_OFFSET,
    PROP_VERSION,
    PROP_ROOT,
    N_PROPERTIES,
};

static GParamSpec *obj_properties[N_PROPERTIES];

// GObject /////////////////////////////////////////////////////////////////////

static void rmf_loader_dispose(GObject *object)
{
    auto const self = RMF_LOADER(object);
    if (self->data) {
        g_bytes_unref(self->data);
        self->data = nullptr;
    }
    if (self->tag_stack) {
        g_ptr_array_unref(self->tag_stack);
        self->tag_stack = nullptr;
    }
    g_clear_object(&self->root);
    G_OBJECT_CLASS(rmf_loader_parent_class)->dispose(object);
}

static void rmf_loader_finalize(GObject *object)
{
    auto const self = RMF_LOADER(object);
    g_free((gpointer)self->source);
    G_OBJECT_CLASS(rmf_loader_parent_class)->finalize(object);
}

static void rmf_loader_get_property(
    GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec
)
{
    auto const self = RMF_LOADER(object);
    switch ((enum RmfLoaderProperty)property_id) {
    case PROP_OFFSET:
        g_value_set_int64(value, self->offset);
        break;
    case PROP_VERSION:
        g_value_set_float(value, self->version);
        break;
    case PROP_ROOT:
        g_value_set_object(value, self->root);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void rmf_loader_set_property(
    GObject *object,
    guint property_id,
    GValue const *value,
    GParamSpec *pspec
)
{
    auto const self = RMF_LOADER(object);
    switch ((enum RmfLoaderProperty)property_id) {
    case PROP_SOURCE:
        g_free((gpointer)self->source);
        self->source = g_value_dup_string(value);
        break;
    case PROP_DATA:
        g_bytes_unref(self->data);
        self->data = g_value_dup_boxed(value);
        break;
    case PROP_OFFSET:
        self->offset = g_value_get_int64(value);
        break;
    case PROP_ROOT:
        self->root = g_value_dup_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

// Boilerplate /////////////////////////////////////////////////////////////////

static void rmf_loader_class_init(RmfLoaderClass *klass)
{
    auto const oclass = G_OBJECT_CLASS(klass);
    oclass->dispose = rmf_loader_dispose;
    oclass->finalize = rmf_loader_finalize;
    oclass->get_property = rmf_loader_get_property;
    oclass->set_property = rmf_loader_set_property;

    /**
     * RmfLoader:source
     *
     * Human-readable name for the data source, for use in error messages.
     */
    obj_properties[PROP_SOURCE] = g_param_spec_string(
        "source",
        nullptr,
        "String identifying the source of the data (eg. filename).",
        "",
        G_PARAM_WRITABLE
    );

    /**
     * RmfLoader:data
     *
     * The source data.
     */
    obj_properties[PROP_DATA] = g_param_spec_boxed(
        "data",
        nullptr,
        "GBytes object containing the RMF data.",
        G_TYPE_BYTES,
        G_PARAM_WRITABLE
    );

    /**
     * RmfLoader:offset
     *
     * The current byte offset into the data.
     */
    obj_properties[PROP_OFFSET] = g_param_spec_int64(
        "offset",
        nullptr,
        "Current offset into data.",
        G_MINOFFSET,
        G_MAXOFFSET,
        0,
        G_PARAM_READWRITE
    );

    /**
     * RmfLoader:version
     *
     * RMF version of the loaded data.
     */
    obj_properties[PROP_VERSION] = g_param_spec_float(
        "version",
        nullptr,
        "RMF version.",
        RMF_MIN_SUPPORTED_VERSION,
        RMF_MAX_SUPPORTED_VERSION,
        RMF_MAX_SUPPORTED_VERSION,
        G_PARAM_READABLE
    );

    /**
     * RmfLoader:root
     *
     * Toplevel object for the currently loaded data.
     */
    obj_properties[PROP_ROOT] = g_param_spec_object(
        "root",
        nullptr,
        nullptr,
        RMF_TYPE_ROOT,
        G_PARAM_READWRITE
    );

    g_object_class_install_properties(oclass, N_PROPERTIES, obj_properties);
}

static void rmf_loader_init(RmfLoader *self)
{
    self->tag_stack = g_ptr_array_new();
}

// Public //////////////////////////////////////////////////////////////////////

/**
 * rmf_loader_new:
 *
 * Creates a new [class@RmfLoader].
 *
 * Returns: The new [class@RmfLoader].
 */
RmfLoader *rmf_loader_new(void)
{
    return g_object_new(RMF_TYPE_LOADER, nullptr);
}

/**
 * rmf_loader_load_from_file:
 * @loader: The loader.
 * @file: File to source the data from.
 * @error: Return location for [a recoverable
 * error](https://docs.gtk.org/glib/error-reporting.html#rules-for-use-of-gerror).
 *
 * Load RMF data from a file.
 */
void rmf_loader_load_from_file(RmfLoader *self, GFile *file, GError **error)
{
    g_return_if_fail(RMF_IS_LOADER(self));
    g_return_if_fail(G_IS_FILE(file));
    g_return_if_fail(error == nullptr || *error == nullptr);

    g_autoptr(GBytes) data = g_file_load_bytes(file, nullptr, nullptr, error);
    if (error && *error) {
        return;
    }
    g_autofree char *filename = g_file_get_path(file);
    g_autofree char *source = g_filename_display_basename(filename);

    g_object_set(self, "source", source, "data", data, nullptr);
    rmf_loader_set_offset(self, 0);

    rmf_read_float(self, &self->version);
    if (self->version < RMF_MIN_SUPPORTED_VERSION
        || self->version > RMF_MAX_SUPPORTED_VERSION)
    {
        g_printerr(
            "Unsupported RMF version %g (only versions %g through %g are supported)",
            self->version,
            RMF_MIN_SUPPORTED_VERSION,
            RMF_MAX_SUPPORTED_VERSION
        );
    }

    char magic[3];
    rmf_loader_read(self, 3, magic);
    if (memcmp(magic, "RMF", 3) != 0) {
        g_printerr("Invalid RMF magic number \"%.3s\"\n", magic);
    }

    rmf_loader_log_begin(self, "rmf", "version", "%g", self->version, nullptr);
    auto root = rmf_root_new(self);
    g_object_set(self, "root", root, nullptr);
    rmf_loader_log_end(self);
}

/**
 * rmf_loader_get_root:
 * @loader: The loader.
 *
 * Get the toplevel object for the currently loaded data.
 *
 * Returns: (transfer none): The loaded toplevel object.
 */
RmfRoot *rmf_loader_get_root(RmfLoader *self)
{
    RmfRoot *value = nullptr;
    g_object_get(self, "root", &value, nullptr);
    return value;
}

/**
 * rmf_loader_get_version:
 * @loader: The loader.
 *
 * Get the RMF version number of the currently loaded data.
 *
 * Returns: The RMF version of the loaded data.
 */
rmf_float rmf_loader_get_version(RmfLoader *self)
{
    rmf_float version = 0.f;
    g_object_get(self, "version", &version, nullptr);
    return version;
}

// Internal ////////////////////////////////////////////////////////////////////

void rmf_loader_set_offset(RmfLoader *self, size_t offset)
{
    g_object_set(self, "offset", offset, nullptr);
}

void rmf_loader_seek(RmfLoader *self, goffset n)
{
    rmf_loader_set_offset(self, self->offset + n);
}

void rmf_loader_read(RmfLoader *self, size_t n, void *dest)
{
    g_return_if_fail(dest);
    auto const src = g_bytes_get_region(self->data, 1, self->offset, n);
    memcpy(dest, src, n);
    self->offset += n;
}

// Helper for rmf_loader_log_* funcs
static char *make_tag(char const *tag, va_list ap)
{
    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
    g_strv_builder_add(builder, tag);

    for (char const *attr = va_arg(ap, char const *); attr != nullptr;
         attr = va_arg(ap, char const *))
    {
        char const *attrfmt = va_arg(ap, char const *);
        g_autofree auto fmt
            = g_strjoin(nullptr, attr, "=\"", attrfmt, "\"", nullptr);
        g_strv_builder_take(builder, g_strdup_vprintf(fmt, ap));
    }

    g_auto(GStrv) strv = g_strv_builder_end(builder);
    auto result = g_strjoinv(" ", strv);

    return result;
}

static char *indent(unsigned int n, char const *str)
{
    constexpr unsigned int INDENT_WIDTH = 2;

    g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
    for (unsigned int i = 0; i < n * INDENT_WIDTH; ++i) {
        g_strv_builder_add(builder, " ");
    }
    g_strv_builder_add(builder, str);

    g_auto(GStrv) str_array = g_strv_builder_end(builder);
    auto s = g_strjoinv(nullptr, str_array);
    return s;
}

void rmf_loader_log_begin(RmfLoader *self, char const *tag, ...)
{
    g_ptr_array_add(self->tag_stack, (void *)tag);

    va_list ap;
    va_start(ap, tag);
    g_autofree auto result = make_tag(tag, ap);
    va_end(ap);

    g_autofree auto xml = g_strdup_printf("<%s>", result);
    g_autofree auto indented = indent(self->tag_stack->len - 1, xml);
    g_info("%s+%08" G_GOFFSET_FORMAT "x: %s", self->source, self->offset, indented);
}

void rmf_loader_log_oneline(
    RmfLoader *self,
    char const *tag,
    char const *content,
    ...
)
{
    va_list ap;
    va_start(ap, content);
    g_autofree auto result = make_tag(tag, ap);
    va_end(ap);

    g_autofree char *xml = nullptr;
    if (content == nullptr) {
        xml = g_strdup_printf("<%s/>", result);
    } else {
        xml = g_strdup_printf("<%s>%s</%s>", result, content, tag);
    }
    g_autofree auto indented = indent(self->tag_stack->len, xml);
    g_info("%s+%08" G_GOFFSET_FORMAT "x: %s", self->source, self->offset, indented);
}

void rmf_loader_log_end(RmfLoader *self)
{
    char *tag
        = g_ptr_array_remove_index(self->tag_stack, self->tag_stack->len - 1);
    g_autofree auto xml = g_strdup_printf("</%s>", tag);
    g_autofree auto indented = indent(self->tag_stack->len, xml);
    g_info("%s+%08" G_GOFFSET_FORMAT "x: %s", self->source, self->offset, indented);
}
