/*
 * ngx_http_fancyindex_module.c
 * Copyright © 2007 Adrian Perez <adrianperez@udc.es>
 *
 * Module used for fancy indexing of directories. Features and differences
 * with the stock nginx autoindex module:
 *
 *  - Output is a table instead of a <pre> element with embedded <a> links.
 *  - Header and footer may be added to every generated directory listing.
 *  - Default header and/or footer are generated if custom ones are not
 *    configured. Files used for header and footer can only be local path
 *    names (i.e. you cannot insert the result of a subrequest.)
 *  - Proper HTML is generated: it should validate both as XHTML 1.0 Strict
 *    and HTML 4.01.
 *
 * Base functionality heavy based upon the stock nginx autoindex module,
 * which in turn was made by Igor Sysoev, like the majority of nginx.
 *
 * Distributed under terms of the BSD license.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_log.h>

#include "template.h"

#if defined(__GNUC__) && (__GNUC__ >= 3)
# define ngx_force_inline __attribute__((__always_inline__))
#else /* !__GNUC__ */
# define ngx_force_inline
#endif /* __GNUC__ */


/*
 * Flags used for the fancyindex_readme_mode directive.
 */
#define NGX_HTTP_FANCYINDEX_README_ASIS   0x01
#define NGX_HTTP_FANCYINDEX_README_TOP    0x02
#define NGX_HTTP_FANCYINDEX_README_BOTTOM 0x04
#define NGX_HTTP_FANCYINDEX_README_DIV    0x08
#define NGX_HTTP_FANCYINDEX_README_IFRAME 0x10
#define NGX_HTTP_FANCYINDEX_README_PRE    0x20


/**
 * Configuration structure for the fancyindex module. The configuration
 * commands defined in the module do fill in the members of this structure.
 */
typedef struct {
    ngx_flag_t enable;       /**< Module is enabled. */
    ngx_flag_t localtime;    /**< File mtime dates are sent in local time. */
    ngx_flag_t exact_size;   /**< Sizes are sent always in bytes. */

    ngx_str_t  header;       /**< File name for header, or empty if none. */
	ngx_str_t  css_url;		 /**< URL for custom css stylesheet, or empty if none. */
    ngx_str_t  footer;       /**< File name for footer, or empty if none. */
    ngx_str_t  readme;       /**< File name for readme, or empty if none. */

    ngx_uint_t readme_flags; /**< Options for readme file inclusion. */
} ngx_http_fancyindex_loc_conf_t;


#define NGX_HTTP_FANCYINDEX_PREALLOCATE  50
#define NGX_HTTP_FANCYINDEX_NAME_LEN     50


/**
 * Calculates the length of a NULL-terminated string. It is ugly having to
 * remember to substract 1 from the sizeof result.
 */
#define ngx_sizeof_ssz(_s)  (sizeof(_s) - 1)


/**
 * Copy a static zero-terminated string. Useful to output template
 * string pieces into a temporary buffer.
 */
#define ngx_cpymem_ssz(_p, _t) \
	(ngx_cpymem((_p), (_t), sizeof(_t) - 1))

/**
 * Copy a ngx_str_t.
 */
#define ngx_cpymem_str(_p, _s) \
	(ngx_cpymem((_p), (_s).data, (_s).len))

/**
 * Check whether a particular bit is set in a particular value.
 */
#define ngx_has_flag(_where, _what) \
	(((_where) & (_what)) == (_what))




typedef struct {
    ngx_str_t      name;
    size_t         utf_len;
    ngx_uint_t     escape;
    ngx_uint_t     dir;
    time_t         mtime;
    off_t          size;
} ngx_http_fancyindex_entry_t;



static ngx_inline ngx_str_t
    get_readme_path(ngx_http_request_t *r, const ngx_str_t *last);

static int ngx_libc_cdecl
    ngx_http_fancyindex_cmp_entries(const void *one, const void *two);

static ngx_int_t ngx_http_fancyindex_error(ngx_http_request_t *r,
    ngx_dir_t *dir, ngx_str_t *name);

static ngx_int_t ngx_http_fancyindex_init(ngx_conf_t *cf);

static void *ngx_http_fancyindex_create_loc_conf(ngx_conf_t *cf);

static char *ngx_http_fancyindex_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

/*
 * These are used only once per handler invocation. We can tell GCC to
 * inline them always, if possible (see how ngx_force_inline is defined
 * above).
 */
static ngx_inline ngx_buf_t*
    make_header_buf(ngx_http_request_t *r, ngx_http_fancyindex_loc_conf_t *alcf)
    ngx_force_inline;

static ngx_inline ngx_buf_t*
    make_header_buf(ngx_http_request_t *r, ngx_http_fancyindex_loc_conf_t *alcf)
    ngx_force_inline;

static ngx_inline ngx_buf_t*
    make_footer_buf(ngx_http_request_t *r, ngx_http_fancyindex_loc_conf_t *alcf)
    ngx_force_inline;



static const ngx_conf_bitmask_t ngx_http_fancyindex_readme_flags[] = {
    { ngx_string("pre"),    NGX_HTTP_FANCYINDEX_README_PRE    },
    { ngx_string("asis"),   NGX_HTTP_FANCYINDEX_README_ASIS   },
    { ngx_string("top"),    NGX_HTTP_FANCYINDEX_README_TOP    },
    { ngx_string("bottom"), NGX_HTTP_FANCYINDEX_README_BOTTOM },
    { ngx_string("div"),    NGX_HTTP_FANCYINDEX_README_DIV    },
    { ngx_string("iframe"), NGX_HTTP_FANCYINDEX_README_IFRAME },
    { ngx_null_string,      0                                 },
};



static ngx_command_t  ngx_http_fancyindex_commands[] = {

    { ngx_string("fancyindex"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, enable),
      NULL },

	{ ngx_string("fancyindex_css_url"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, css_url),
      NULL },

    { ngx_string("fancyindex_localtime"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, localtime),
      NULL },

    { ngx_string("fancyindex_exact_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, exact_size),
      NULL },

    { ngx_string("fancyindex_header"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, header),
      NULL },

    { ngx_string("fancyindex_footer"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, footer),
      NULL },

    { ngx_string("fancyindex_readme"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, readme),
      NULL },

    { ngx_string("fancyindex_readme_mode"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_conf_set_bitmask_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_fancyindex_loc_conf_t, readme_flags),
      &ngx_http_fancyindex_readme_flags },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_fancyindex_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_fancyindex_init,              /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_fancyindex_create_loc_conf,   /* create location configration */
    ngx_http_fancyindex_merge_loc_conf     /* merge location configration */
};


ngx_module_t  ngx_http_fancyindex_module = {
    NGX_MODULE_V1,
    &ngx_http_fancyindex_module_ctx,       /* module context */
    ngx_http_fancyindex_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};




static ngx_inline ngx_buf_t*
make_header_buf(ngx_http_request_t *r, ngx_http_fancyindex_loc_conf_t *alcf)
{
    size_t blen = r->uri.len
        + ngx_sizeof_ssz(t01_head1)
        + ngx_sizeof_ssz(t02_head2)
		+ ngx_sizeof_ssz(t08_head3)
        + ngx_sizeof_ssz(t03_body1)
        ;
    ngx_buf_t *b = ngx_create_temp_buf(r->pool, blen);

    if (b == NULL) goto bailout;

    b->last = ngx_cpymem_ssz(b->last, t01_head1);
    b->last = ngx_cpymem_str(b->last, r->uri);
    b->last = ngx_cpymem_ssz(b->last, t02_head2);

	/*
	 * If a custom CSS stylesheet is being used add it to the header!
	 */
	if (alcf->css_url != NULL && alcf->css_url.len > 0) {
		b->last = ngx_cpymem_ssz(b->last, "<link href=\"");
		b->last = ngx_cpymem_ssz(b->last, alcf->css_url);
		b->last = ngx_cpymem_ssz(b->last, "\" media=\"screen\" rel=\"stylesheet\" type=\"text/css\" />");
	}
	
	// ending head tag
	b->last = ngx_cpymem_ssz(b->last, t08_head3);
	
    b->last = ngx_cpymem_ssz(b->last, t03_body1);

bailout:
    return b;
}



static ngx_inline ngx_buf_t*
make_footer_buf(ngx_http_request_t *r)
{
    /*
     * TODO: Make this buffer static (i.e. readonly and reusable from
     * one request to another.
     */
    ngx_buf_t *b = ngx_create_temp_buf(r->pool, ngx_sizeof_ssz(t07_foot1));

    if (b == NULL) goto bailout;

    b->last = ngx_cpymem_ssz(b->last, t07_foot1);

bailout:
    return b;
}



static inline ngx_int_t
make_content_buf(
        ngx_http_request_t *r, ngx_buf_t **pb,
        ngx_http_fancyindex_loc_conf_t *alcf)
{
    ngx_http_fancyindex_entry_t *entry;

    off_t        length;
    size_t       len, root, copy, allocated;
    u_char      *filename, *last, scale;
    ngx_tm_t     tm;
    ngx_array_t  entries;
    ngx_time_t  *tp;
    ngx_uint_t   i;
    ngx_int_t    size;
    ngx_str_t    path;
    ngx_str_t    readme_path;
	ngx_str_t	 css_url;
    ngx_dir_t    dir;
    ngx_buf_t   *b;

    static char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    /*
     * NGX_DIR_MASK_LEN is lesser than NGX_HTTP_FANCYINDEX_PREALLOCATE
     */
    if ((last = ngx_http_map_uri_to_path(r, &path, &root,
                    NGX_HTTP_FANCYINDEX_PREALLOCATE)) == NULL)
        return NGX_HTTP_INTERNAL_SERVER_ERROR;

    allocated = path.len;
    path.len  = last - path.data - 1;
    path.data[path.len] = '\0';

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http fancyindex: \"%s\"", path.data);

    if (ngx_open_dir(&path, &dir) == NGX_ERROR) {
        ngx_int_t rc, err = ngx_errno;
        ngx_uint_t level;

        if (err == NGX_ENOENT || err == NGX_ENOTDIR || err == NGX_ENAMETOOLONG) {
            level = NGX_LOG_ERR;
            rc = NGX_HTTP_NOT_FOUND;
        } else if (err == NGX_EACCES) {
            level = NGX_LOG_ERR;
            rc = NGX_HTTP_FORBIDDEN;
        } else {
            level = NGX_LOG_CRIT;
            rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_log_error(level, r->connection->log, err,
                ngx_open_dir_n " \"%s\" failed", path.data);

        return rc;
    }

#if (NGX_SUPPRESS_WARN)
    /* MSVC thinks 'entries' may be used without having been initialized */
    ngx_memzero(&entries, sizeof(ngx_array_t));
#endif /* NGX_SUPPRESS_WARN */


    if (ngx_array_init(&entries, r->pool, 40,
                sizeof(ngx_http_fancyindex_entry_t)) != NGX_OK)
        return ngx_http_fancyindex_error(r, &dir, &path);

    filename = path.data;
    filename[path.len] = '/';

    /* Read directory entries and their associated information. */
    for (;;) {
        ngx_set_errno(0);

        if (ngx_read_dir(&dir) == NGX_ERROR) {
            ngx_int_t err = ngx_errno;

            if (err != NGX_ENOMOREFILES) {
                ngx_log_error(NGX_LOG_CRIT, r->connection->log, err,
                        ngx_read_dir_n " \"%V\" failed", &path);
                return ngx_http_fancyindex_error(r, &dir, &path);
            }
            break;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http fancyindex file: \"%s\"", ngx_de_name(&dir));

        len = ngx_de_namelen(&dir);

        if (ngx_de_name(&dir)[0] == '.')
            continue;

        if (!dir.valid_info) {
            /* 1 byte for '/' and 1 byte for terminating '\0' */
            if (path.len + 1 + len + 1 > allocated) {
                allocated = path.len + 1 + len + 1
                          + NGX_HTTP_FANCYINDEX_PREALLOCATE;

                if ((filename = ngx_palloc(r->pool, allocated)) == NULL)
                    return ngx_http_fancyindex_error(r, &dir, &path);

                last = ngx_cpystrn(filename, path.data, path.len + 1);
                *last++ = '/';
            }

            ngx_cpystrn(last, ngx_de_name(&dir), len + 1);

            if (ngx_de_info(filename, &dir) == NGX_FILE_ERROR) {
                ngx_int_t err = ngx_errno;

                if (err != NGX_ENOENT) {
                    ngx_log_error(NGX_LOG_CRIT, r->connection->log, err,
                            ngx_de_info_n " \"%s\" failed", filename);
                    return ngx_http_fancyindex_error(r, &dir, &path);
                }

                if (ngx_de_link_info(filename, &dir) == NGX_FILE_ERROR) {
                    ngx_log_error(NGX_LOG_CRIT, r->connection->log, ngx_errno,
                            ngx_de_link_info_n " \"%s\" failed", filename);
                    return ngx_http_fancyindex_error(r, &dir, &path);
                }
            }
        }

        if ((entry = ngx_array_push(&entries)) == NULL)
            return ngx_http_fancyindex_error(r, &dir, &path);

        entry->name.len  = len;
        entry->name.data = ngx_palloc(r->pool, len + 1);
        if (entry->name.data == NULL)
            return ngx_http_fancyindex_error(r, &dir, &path);

        ngx_cpystrn(entry->name.data, ngx_de_name(&dir), len + 1);
        entry->escape = 2 * ngx_escape_uri(NULL,
                ngx_de_name(&dir), len, NGX_ESCAPE_HTML);

        entry->dir     = ngx_de_is_dir(&dir);
        entry->mtime   = ngx_de_mtime(&dir);
        entry->size    = ngx_de_size(&dir);
        entry->utf_len = (r->headers_out.charset.len == 5 &&
            ngx_strncasecmp(r->headers_out.charset.data, (u_char*) "utf-8", 5) == 0) 
            ?  ngx_utf8_length(entry->name.data, entry->name.len)
            : len;
    }

    if (ngx_close_dir(&dir) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, ngx_errno,
                ngx_close_dir_n " \"%s\" failed", &path);
    }

    /*
     * Calculate needed buffer length.
     */
    len = r->uri.len
        + ngx_sizeof_ssz(t04_body2)
        + ngx_sizeof_ssz(t05_list1)
        + ngx_sizeof_ssz(t06_list2)
        ;

    /*
     * If including an <iframe> for the readme file, add the length of the
     * URI, plus the length of the readme file name and the length of the
     * needed markup.
     */
    readme_path = get_readme_path(r, &path);
    if (readme_path.len > 0) {
        if (ngx_has_flag(alcf->readme_flags, NGX_HTTP_FANCYINDEX_README_IFRAME)) {
            len += 2 /* CR+LF */
                + ngx_sizeof_ssz("<iframe id=\"readme\" src=\"")
                + r->uri.len + alcf->readme.len
                + ngx_sizeof_ssz("\">(readme file)</iframe>")
                ;
        }
        else {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "fancyindex: bad readme_flags combination %#x",
                    alcf->readme_flags);
        }
    }

	/*
	 * If incuding a css stylesheet add the length of the URI, plus the length of
	 * the filename and the needed markup.
	 */
	if (alcf->css_url != NULL && alcf->css_url.len > 0) {
		len += 2 /* CR+LF */
			+ ngx_sizeof_ssz("<link href=\"\" media=\"screen\" rel=\"stylesheet\" type=\"text/css\" />")
			+ alcf->css_url.len
			;
	}

    entry = entries.elts;
    for (i = 0; i < entries.nelts; i++) {
        /*
         * Genearated table rows are as follows, unneeded whitespace
         * is stripped out:
         *
         *   <tr class="X">
         *     <td><a href="U">fname</a></td>
         *     <td>size</td><td>date</td>
         *   </tr>
         */
        len += ngx_sizeof_ssz("<tr class=\"X\"><td><a href=\"")
            + entry[i].name.len + entry[i].escape /* Escaped URL */
            + ngx_sizeof_ssz("\">")
            + entry[i].name.len + entry[i].utf_len
            + NGX_HTTP_FANCYINDEX_NAME_LEN + ngx_sizeof_ssz("&gt;")
            + ngx_sizeof_ssz("</a></td><td>")
            + 20 /* File size */
            + ngx_sizeof_ssz("</td><td>")
            + ngx_sizeof_ssz(" 28-Sep-1970 12:00 ")
            + ngx_sizeof_ssz("</td></tr>\n")
            + 2 /* CR LF */
            ;
    }

    if ((b = ngx_create_temp_buf(r->pool, len)) == NULL)
        return NGX_HTTP_INTERNAL_SERVER_ERROR;

    /* Sort entries, if needed */
    if (entries.nelts > 1) {
        ngx_qsort(entry, (size_t) entries.nelts,
                sizeof(ngx_http_fancyindex_entry_t),
                ngx_http_fancyindex_cmp_entries);
    }

    b->last = ngx_cpymem_str(b->last, r->uri);
    b->last = ngx_cpymem_ssz(b->last, t04_body2);

    /* Insert readme at top, if appropriate */
    if ((readme_path.len == 0) ||
            !ngx_has_flag(alcf->readme_flags, NGX_HTTP_FANCYINDEX_README_TOP))
        goto skip_readme_top;

#define nfi_add_readme_iframe( ) \
        do { \
            b->last = ngx_cpymem_ssz(b->last, "<iframe id=\"readme\" src=\""); \
            b->last = ngx_cpymem_str(b->last, r->uri); \
            b->last = ngx_cpymem_str(b->last, alcf->readme); \
            b->last = ngx_cpymem_ssz(b->last, "\">(readme file)</iframe>"); \
            *b->last++ = CR; \
            *b->last++ = LF; \
        } while (0)

    if (ngx_has_flag(alcf->readme_flags, NGX_HTTP_FANCYINDEX_README_IFRAME))
        nfi_add_readme_iframe();
    else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "fancyindex: bad readme_flags combination %#x",
                alcf->readme_flags);
    }
skip_readme_top:

    /* Output table header */
    b->last = ngx_cpymem_ssz(b->last, t05_list1);

    tp = ngx_timeofday();

    for (i = 0; i < entries.nelts; i++) {
        static const char _evenodd[] = { 'e', 'o' };
        b->last = ngx_cpymem_ssz(b->last, "<tr class=\"");
        *b->last++ = _evenodd[i & 0x01];
        /*
         * Alternative implementation:
         *   *b->last++ = (i & 0x01) ? 'e' : 'o';
         */
        b->last = ngx_cpymem_ssz(b->last, "\"><td><a href=\"");

        if (entry[i].escape) {
            ngx_escape_uri(b->last, entry[i].name.data, entry[i].name.len,
                           NGX_ESCAPE_HTML);

            b->last += entry[i].name.len + entry[i].escape;

        } else {
            b->last = ngx_cpymem_str(b->last, entry[i].name);
        }

        if (entry[i].dir) {
            *b->last++ = '/';
        }

        *b->last++ = '"';
        *b->last++ = '>';

        len = entry[i].utf_len;

        if (entry[i].name.len - len) {
            if (len > NGX_HTTP_FANCYINDEX_NAME_LEN) {
                copy = NGX_HTTP_FANCYINDEX_NAME_LEN - 3 + 1;
            } else {
                copy = NGX_HTTP_FANCYINDEX_NAME_LEN + 1;
            }

            b->last = ngx_utf8_cpystrn(b->last, entry[i].name.data, copy, NGX_HTTP_FANCYINDEX_NAME_LEN + 1);
            last = b->last;

        } else {
            b->last = ngx_cpystrn(b->last, entry[i].name.data,
                                  NGX_HTTP_FANCYINDEX_NAME_LEN + 1);
            last = b->last - 3;
        }

        if (len > NGX_HTTP_FANCYINDEX_NAME_LEN) {
            b->last = ngx_cpymem_ssz(last, "..&gt;</a></td><td>");

        } else {
            if (entry[i].dir && NGX_HTTP_FANCYINDEX_NAME_LEN - len > 0) {
                *b->last++ = '/';
                len++;
            }

            b->last = ngx_cpymem_ssz(b->last, "</a></td><td>");
        }

        if (alcf->exact_size) {
            if (entry[i].dir) {
                *b->last++ = '-';
            } else {
                b->last = ngx_sprintf(b->last, "%19O", entry[i].size);
            }

        } else {
            if (entry[i].dir) {
                *b->last++ = '-';
            } else {
                length = entry[i].size;

                if (length > 1024 * 1024 * 1024 - 1) {
                    size = (ngx_int_t) (length / (1024 * 1024 * 1024));
                    if ((length % (1024 * 1024 * 1024))
                                                > (1024 * 1024 * 1024 / 2 - 1))
                    {
                        size++;
                    }
                    scale = 'G';

                } else if (length > 1024 * 1024 - 1) {
                    size = (ngx_int_t) (length / (1024 * 1024));
                    if ((length % (1024 * 1024)) > (1024 * 1024 / 2 - 1)) {
                        size++;
                    }
                    scale = 'M';

                } else if (length > 9999) {
                    size = (ngx_int_t) (length / 1024);
                    if (length % 1024 > 511) {
                        size++;
                    }
                    scale = 'K';

                } else {
                    size = (ngx_int_t) length;
                    scale = '\0';
                }

                if (scale) {
                    b->last = ngx_sprintf(b->last, "%6i%c", size, scale);

                } else {
                    b->last = ngx_sprintf(b->last, " %6i", size);
                }
            }
        }

        ngx_gmtime(entry[i].mtime + tp->gmtoff * 60 * alcf->localtime, &tm);

        b->last = ngx_sprintf(b->last, "</td><td>%02d-%s-%d %02d:%02d</td></tr>",
                              tm.ngx_tm_mday,
                              months[tm.ngx_tm_mon - 1],
                              tm.ngx_tm_year,
                              tm.ngx_tm_hour,
                              tm.ngx_tm_min);


        *b->last++ = CR;
        *b->last++ = LF;
    }

    /* Output table bottom */
    b->last = ngx_cpymem_ssz(b->last, t06_list2);

    /* Insert readme at bottom, if appropriate */
    if ((readme_path.len == 0) ||
            ngx_has_flag(alcf->readme_flags, NGX_HTTP_FANCYINDEX_README_TOP) ||
            !ngx_has_flag(alcf->readme_flags, NGX_HTTP_FANCYINDEX_README_BOTTOM))
        goto skip_readme_bottom;

    if (ngx_has_flag(alcf->readme_flags, NGX_HTTP_FANCYINDEX_README_IFRAME))
        nfi_add_readme_iframe();
    else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "fancyindex: bad readme_flags combination %#x",
                alcf->readme_flags);
    }
skip_readme_bottom:

    *pb = b;
    return NGX_OK;
}



static ngx_int_t
ngx_http_fancyindex_handler(ngx_http_request_t *r)
{
    ngx_http_request_t             *sr;
    ngx_str_t                      *sr_uri;
    ngx_str_t                       rel_uri;
    ngx_int_t                       rc;
    ngx_http_fancyindex_loc_conf_t *alcf;
    ngx_chain_t                     out[3] = {
        { NULL, NULL }, { NULL, NULL}, { NULL, NULL }};


    if (r->uri.data[r->uri.len - 1] != '/') {
        return NGX_DECLINED;
    }

    /* TODO: Win32 */
    if (r->zero_in_uri) {
        return NGX_DECLINED;
    }

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_DECLINED;
    }

    alcf = ngx_http_get_module_loc_conf(r, ngx_http_fancyindex_module);

    if (!alcf->enable) {
        return NGX_DECLINED;
    }

    if ((rc = make_content_buf(r, &out[0].buf, alcf) != NGX_OK))
        return rc;

    out[0].buf->last_in_chain = 1;

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_type_len  = ngx_sizeof_ssz("text/html");
    r->headers_out.content_type.len  = ngx_sizeof_ssz("text/html");
    r->headers_out.content_type.data = (u_char *) "text/html";

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }


    if (alcf->header.len > 0) {
        /* URI is configured, make Nginx take care of with a subrequest. */
        sr_uri = &alcf->header;

        if (*sr_uri->data != '/') {
            /* Relative path */
            rel_uri.len  = r->uri.len + alcf->header.len;
            rel_uri.data = ngx_palloc(r->pool, rel_uri.len);
            if (rel_uri.data == NULL) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            ngx_memcpy(ngx_cpymem(rel_uri.data, r->uri.data, r->uri.len),
                    alcf->header.data, alcf->header.len);
            sr_uri = &rel_uri;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "http fancyindex: header subrequest \"%V\"", sr_uri);

        rc = ngx_http_subrequest(r, sr_uri, NULL, &sr, NULL, 0);
        if (rc == NGX_ERROR || rc == NGX_DONE) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "http fancyindex: header subrequest for \"%V\" failed", sr_uri);
            return rc;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "http fancyindex: header subrequest status = %i",
                sr->headers_out.status);

        if (sr->headers_out.status != NGX_HTTP_OK) {
            /*
             * XXX: Should we write a message to the error log just in case
             * we get something different from a 404?
             */
            goto add_builtin_header;
        }
    }
    else {
add_builtin_header:
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "http fancyindex: adding built-in header");
        /* Make space before */
        out[1].next = out[0].next;
        out[1].buf  = out[0].buf;
        /* Chain header buffer */
        out[0].next = &out[1];
        out[0].buf  = make_header_buf(r, alcf);
    }

    /* If footer is disabled, chain up footer buffer. */
    if (alcf->footer.len == 0) {
        ngx_uint_t last  = (alcf->header.len == 0) ? 2 : 1;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "http fancyindex: adding built-in footer at %i", last);

        out[last-1].next = &out[last];
        out[last].buf    = make_footer_buf(r);

        out[last-1].buf->last_in_chain = 0;
        out[last].buf->last_in_chain   = 1;
        out[last].buf->last_buf        = 1;
        /* Send everything with a single call :D */
        return ngx_http_output_filter(r, &out[0]);
    }

    /*
     * If we reach here, we were asked to send a custom footer. We need to:
     * partially send whatever is referenced from out[0] and then send the
     * footer as a subrequest. If the subrequest fails, we should send the
     * standard footer as well.
     */
    rc = ngx_http_output_filter(r, &out[0]);

    if (rc != NGX_OK && rc != NGX_AGAIN)
        return NGX_HTTP_INTERNAL_SERVER_ERROR;

    /* URI is configured, make Nginx take care of with a subrequest. */
    sr_uri = &alcf->footer;

    if (*sr_uri->data != '/') {
        /* Relative path */
        rel_uri.len  = r->uri.len + alcf->footer.len;
        rel_uri.data = ngx_palloc(r->pool, rel_uri.len);
        if (rel_uri.data == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        ngx_memcpy(ngx_cpymem(rel_uri.data, r->uri.data, r->uri.len),
                alcf->footer.data, alcf->footer.len);
        sr_uri = &rel_uri;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "http fancyindex: footer subrequest \"%V\"", sr_uri);

    rc = ngx_http_subrequest(r, sr_uri, NULL, &sr, NULL, 0);
    if (rc == NGX_ERROR || rc == NGX_DONE) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "http fancyindex: footer subrequest for \"%V\" failed", sr_uri);
        return rc;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "http fancyindex: header subrequest status = %i",
            sr->headers_out.status);

    if (sr->headers_out.status != NGX_HTTP_OK) {
        /*
         * XXX: Should we write a message to the error log just in case
         * we get something different from a 404?
         */
        out[0].next = NULL;
        out[0].buf  = make_footer_buf(r);
        out[0].buf->last_in_chain = 1;
        out[0].buf->last_buf = 1;
        /* Directly send out the builtin footer */
        return ngx_http_output_filter(r, &out[0]);
    }

    return (r != r->main) ? rc : ngx_http_send_special(r, NGX_HTTP_LAST);
}


static int ngx_libc_cdecl
ngx_http_fancyindex_cmp_entries(const void *one, const void *two)
{
    ngx_http_fancyindex_entry_t *first = (ngx_http_fancyindex_entry_t *) one;
    ngx_http_fancyindex_entry_t *second = (ngx_http_fancyindex_entry_t *) two;

    if (first->dir && !second->dir) {
        /* move the directories to the start */
        return -1;
    }

    if (!first->dir && second->dir) {
        /* move the directories to the start */
        return 1;
    }

    return (int) ngx_strcmp(first->name.data, second->name.data);
}



static ngx_inline ngx_str_t
get_readme_path(ngx_http_request_t *r, const ngx_str_t *path)
{
    u_char *last;
    ngx_file_info_t info;
    ngx_str_t fullpath = ngx_null_string;
    ngx_http_fancyindex_loc_conf_t *alcf =
        ngx_http_get_module_loc_conf(r, ngx_http_fancyindex_module);

    if (alcf->readme.len == 0) /* Readme files are disabled */
        return fullpath;

    fullpath.len  = path->len + 2 + alcf->readme.len;
    fullpath.data = ngx_palloc(r->pool, fullpath.len);

    last = ngx_cpymem_str(fullpath.data, *path); *last++ = '/';
    last = ngx_cpymem_str(last, alcf->readme); *last++ = '\0';

    /* File does not exists, or cannot be accessed */
    if (ngx_file_info(fullpath.data, &info) != 0)
        fullpath.len = 0;

    return fullpath;
}


static ngx_int_t
ngx_http_fancyindex_error(ngx_http_request_t *r, ngx_dir_t *dir, ngx_str_t *name)
{
    if (ngx_close_dir(dir) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, ngx_errno,
                      ngx_close_dir_n " \"%V\" failed", name);
    }

    return NGX_HTTP_INTERNAL_SERVER_ERROR;
}


static void *
ngx_http_fancyindex_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_fancyindex_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_fancyindex_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * Set by ngx_pcalloc:
     *    conf->header.len   = 0
     *    conf->header.data  = NULL
     *    conf->footer.len   = 0
     *    conf->footer.data  = NULL
     *    conf->readme.len   = 0
     *    conf->readme.data  = NULL
     *    conf->readme_flags = 0
     */
    conf->enable = NGX_CONF_UNSET;
    conf->localtime = NGX_CONF_UNSET;
    conf->exact_size = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_fancyindex_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_fancyindex_loc_conf_t *prev = parent;
    ngx_http_fancyindex_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->localtime, prev->localtime, 0);
    ngx_conf_merge_value(conf->exact_size, prev->exact_size, 1);

    ngx_conf_merge_str_value(conf->header, prev->header, "");
    ngx_conf_merge_str_value(conf->footer, prev->footer, "");
    ngx_conf_merge_str_value(conf->readme, prev->readme, "");

    ngx_conf_merge_bitmask_value(conf->readme_flags, prev->readme_flags,
            NGX_HTTP_FANCYINDEX_README_TOP);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_fancyindex_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_fancyindex_handler;

    return NGX_OK;
}

/* vim:et:sw=4:ts=4:
 */
