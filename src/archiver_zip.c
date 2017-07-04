/*
 * ZIP support routines for PhysicsFS.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon, with some peeking at "unzip.c"
 *   by Gilles Vollant.
 */

#define __PHYSICSFS_INTERNAL__
#include "physfs_internal.h"

#if PHYSFS_SUPPORTS_ZIP

#include <errno.h>
#include <time.h>

#include "physfs_miniz.h"
#include "aes/fileenc.h"

/*
 * A buffer of ZIP_READBUFSIZE is allocated for each compressed file opened,
 *  and is freed when you close the file; compressed data is read into
 *  this buffer, and then is decompressed into the buffer passed to
 *  PHYSFS_read().
 *
 * Uncompressed entries in a zipfile do not allocate this buffer; they just
 *  read data directly into the buffer passed to PHYSFS_read().
 *
 * Depending on your speed and memory requirements, you should tweak this
 *  value.
 */
#define ZIP_READBUFSIZE   (16 * 1024)


/*
 * Entries are "unresolved" until they are first opened. At that time,
 *  local file headers parsed/validated, data offsets will be updated to look
 *  at the actual file data instead of the header, and symlinks will be
 *  followed and optimized. This means that we don't seek and read around the
 *  archive until forced to do so, and after the first time, we had to do
 *  less reading and parsing, which is very CD-ROM friendly.
 */
typedef enum
{
    ZIP_UNRESOLVED_FILE,
    ZIP_UNRESOLVED_SYMLINK,
    ZIP_RESOLVING,
    ZIP_RESOLVED,
    ZIP_DIRECTORY,
    ZIP_BROKEN_FILE,
    ZIP_BROKEN_SYMLINK
} ZipResolveType;

typedef struct _ZIP_AES_Data
{
    PHYSFS_uint8 key_strength;      /* 128, 192 or 256 bit keys       */
    PHYSFS_uint8 salt[16];
    PHYSFS_uint16 pass_verification;
    PHYSFS_uint16 compression;
} ZIP_AES_Data;


/*
 * One ZIPentry is kept for each file in an open ZIP archive.
 */
typedef struct _ZIPentry
{
    char *name;                         /* Name of file in archive        */
    struct _ZIPentry *symlink;          /* NULL or file we symlink to     */
    ZipResolveType resolved;            /* Have we resolved file/symlink? */
    PHYSFS_uint64 offset;               /* offset of data in archive      */
    PHYSFS_uint16 version;              /* version made by                */
    PHYSFS_uint16 version_needed;       /* version needed to extract      */
    PHYSFS_uint16 general_bits;         /* general purpose bits           */
    PHYSFS_uint16 compression_method;   /* compression method             */
    PHYSFS_uint32 crc;                  /* crc-32                         */
    PHYSFS_uint64 compressed_size;      /* compressed size                */
    PHYSFS_uint64 uncompressed_size;    /* uncompressed size              */
    PHYSFS_sint64 last_mod_time;        /* last file mod time             */
    PHYSFS_uint32 dos_mod_time;         /* original MS-DOS style mod time */
    struct _ZIPentry *hashnext;         /* next item in this hash bucket  */
    struct _ZIPentry *children;         /* linked list of kids, if dir    */
    struct _ZIPentry *sibling;          /* next item in same dir          */
    ZIP_AES_Data aes_data;
} ZIPentry;

/*
 * One ZIPinfo is kept for each open ZIP archive.
 */
typedef struct
{
    PHYSFS_Io *io;            /* the i/o interface for this archive.    */
    ZIPentry root;            /* root of directory tree.                */
    ZIPentry **hash;          /* all entries hashed for fast lookup.    */
    size_t hashBuckets;       /* number of buckets in hash.             */
    int zip64;                /* non-zero if this is a Zip64 archive.   */
    int has_crypto;           /* non-zero if any entry uses encryption. */
} ZIPinfo;

/*
 * One ZIPfileinfo is kept for each open file in a ZIP archive.
 */
typedef struct
{
    ZIPentry *entry;                      /* Info on file.              */
    PHYSFS_Io *io;                        /* physical file handle.      */
    PHYSFS_uint32 compressed_position;    /* offset in compressed data. */
    PHYSFS_uint32 uncompressed_position;  /* tell() position.           */
    PHYSFS_uint8 *buffer;                 /* decompression buffer.      */
    PHYSFS_uint32 crypto_keys[3];         /* for "traditional" crypto.  */
    PHYSFS_uint32 initial_crypto_keys[3]; /* for "traditional" crypto.  */
    z_stream stream;                      /* zlib stream state.         */
    fcrypt_ctx aes_ctx;

} ZIPfileinfo;


/* Magic numbers... */
#define ZIP_LOCAL_FILE_SIG                          0x04034b50
#define ZIP_CENTRAL_DIR_SIG                         0x02014b50
#define ZIP_END_OF_CENTRAL_DIR_SIG                  0x06054b50
#define ZIP64_END_OF_CENTRAL_DIR_SIG                0x06064b50
#define ZIP64_END_OF_CENTRAL_DIRECTORY_LOCATOR_SIG  0x07064b50
#define ZIP64_EXTENDED_INFO_EXTRA_FIELD_SIG         0x0001

#define ZIP_AES_HEADER_EXTRA_FIELD_SIG              0x9901
#define ZIP_AE1_VENDOR_VERSION		                0x0001
#define ZIP_AE2_VENDOR_VERSION	                    0x0002
#define ZIP_AES_VENDOR_ID			                0x4541 /* 'AE' little endian */
#define ZIP_AES_128_BITS			                0x01
#define ZIP_AES_192_BITS			                0x02
#define ZIP_AES_256_BITS	                        0x03
#define ZIP_IS_AES(entry) (entry->aes_data.key_strength > ZIP_AES_128_BITS)

// this password need to be in sync with the buildbot that packs the files
// please note to note have '"% in there as it breaks the buildbot
#define ZIP_AES_DEFAULT_PASSWORD "8!*MJw=g4e)ah#0BxlcUjl7p*W6jSV!l4qg!31gutTjh.cwJflgfWcd8LhdjaIY0*UYda3Yj@BY9WA"


/* compression methods... */
#define COMPMETH_NONE 0
#define COMPMETH_AES 99 /* Not a real compression dont use it, only used for describe AES encryption */
/* ...and others... */


#define UNIX_FILETYPE_MASK    0170000
#define UNIX_FILETYPE_SYMLINK 0120000

#define ZIP_GENERAL_BITS_TRADITIONAL_CRYPTO   (1 << 0)
#define ZIP_GENERAL_BITS_IGNORE_LOCAL_HEADER  (1 << 3)

/* support for "traditional" PKWARE encryption. */
static int zip_entry_is_tradional_crypto(const ZIPentry *entry)
{
    return (entry->general_bits & ZIP_GENERAL_BITS_TRADITIONAL_CRYPTO) != 0;
} /* zip_entry_is_traditional_crypto */

static int zip_entry_update_aes_offset(ZIPfileinfo *finfo)
{
    ZIPentry *entry = finfo->entry;
    PHYSFS_uint16 pass_verifier;

    fcrypt_init(entry->aes_data.key_strength, ZIP_AES_DEFAULT_PASSWORD, strlen(ZIP_AES_DEFAULT_PASSWORD), entry->aes_data.salt, (unsigned char *)&pass_verifier, &finfo->aes_ctx);
    BAIL_IF_MACRO(entry->aes_data.pass_verification != pass_verifier, PHYSFS_ERR_CORRUPT, 0);

    {
        fcrypt_ctx *cx = &finfo->aes_ctx;
        unsigned long i = 0;
        for (; i < finfo->uncompressed_position; ++i, ++cx->encr_pos)
        {
            if (cx->encr_pos == AES_BLOCK_SIZE)
            {
                unsigned int j = 0;
                /* increment encryption nonce   */
                while (j < 8 && !++cx->nonce[j])
                    ++j;

                /* encrypt the nonce to form next xor buffer    */
                aes_encrypt(cx->nonce, cx->encr_bfr, cx->encr_ctx);
                cx->encr_pos = 0;
            }
        }
    }
    return 1;
}

static int zip_entry_ignore_local_header(const ZIPentry *entry)
{
    return (entry->general_bits & ZIP_GENERAL_BITS_IGNORE_LOCAL_HEADER) != 0;
} /* zip_entry_is_traditional_crypto */

static PHYSFS_uint32 zip_crypto_crc32(const PHYSFS_uint32 crc, const PHYSFS_uint8 val)
{
    int i;
    PHYSFS_uint32 xorval = (crc ^ ((PHYSFS_uint32) val)) & 0xFF;
    for (i = 0; i < 8; i++)
        xorval = ((xorval & 1) ? (0xEDB88320 ^ (xorval >> 1)) : (xorval >> 1));
    return xorval ^ (crc >> 8);
} /* zip_crc32 */

static void zip_update_crypto_keys(PHYSFS_uint32 *keys, const PHYSFS_uint8 val)
{
    keys[0] = zip_crypto_crc32(keys[0], val);
    keys[1] = keys[1] + (keys[0] & 0x000000FF);
    keys[1] = (keys[1] * 134775813) + 1;
    keys[2] = zip_crypto_crc32(keys[2], (PHYSFS_uint8) ((keys[1] >> 24) & 0xFF));
} /* zip_update_crypto_keys */

static PHYSFS_uint8 zip_decrypt_byte(const PHYSFS_uint32 *keys)
{
    const PHYSFS_uint16 tmp = keys[2] | 2;
    return (PHYSFS_uint8) ((tmp * (tmp ^ 1)) >> 8);
} /* zip_decrypt_byte */

static PHYSFS_sint64 zip_read_decrypt(ZIPfileinfo *finfo, void *buf, PHYSFS_uint64 len)
{
    PHYSFS_Io *io = finfo->io;
    const PHYSFS_sint64 br = io->read(io, buf, len);

    /* Decompression the new data if necessary. */
    if (zip_entry_is_tradional_crypto(finfo->entry) && (br > 0))
    {
        if (ZIP_IS_AES(finfo->entry)) {
            if (finfo->aes_ctx.encr_pos > AES_BLOCK_SIZE) {
                if (!zip_entry_update_aes_offset(finfo)) {
                    return -1;
                }
            }
            PHYSFS_sint64 bytesForDecript = br;
            int i = 0;
            while (bytesForDecript >= 16) {
                fcrypt_decrypt(((char*)buf) + i, 16, &finfo->aes_ctx);
                i += 16;
                bytesForDecript -= 16;
            }
            if (bytesForDecript > 0) {
                fcrypt_decrypt(((char*)buf) + i, bytesForDecript, &finfo->aes_ctx);
            }
        }
        else {
            PHYSFS_uint32 *keys = finfo->crypto_keys;
            PHYSFS_uint8 *ptr = (PHYSFS_uint8 *)buf;
            PHYSFS_sint64 i;
            for (i = 0; i < br; i++, ptr++)
            {
                const PHYSFS_uint8 ch = *ptr ^ zip_decrypt_byte(keys);
                zip_update_crypto_keys(keys, ch);
                *ptr = ch;
            } /* for */
        }
    } /* if  */

    return br;
} /* zip_read_decrypt */

static int zip_prep_crypto_keys(ZIPfileinfo *finfo, const PHYSFS_uint8 *crypto_header, const PHYSFS_uint8 *password)
{
    /* It doesn't appear to be documented in PKWare's APPNOTE.TXT, but you
       need to use a different byte in the header to verify the password
       if general purpose bit 3 is set. Discovered this from Info-Zip.
       That's what the (verifier) value is doing, below. */

    PHYSFS_uint32 *keys = finfo->crypto_keys;
    const ZIPentry *entry = finfo->entry;
    const int usedate = zip_entry_ignore_local_header(entry);
    const PHYSFS_uint8 verifier = (PHYSFS_uint8) ((usedate ? (entry->dos_mod_time >> 8) : (entry->crc >> 24)) & 0xFF);
    PHYSFS_uint8 finalbyte = 0;
    int i = 0;

    /* initialize vector with defaults, then password, then header. */
    keys[0] = 305419896;
    keys[1] = 591751049;
    keys[2] = 878082192;

    while (*password)
        zip_update_crypto_keys(keys, *(password++));

    for (i = 0; i < 12; i++)
    {
        const PHYSFS_uint8 c = crypto_header[i] ^ zip_decrypt_byte(keys);
        zip_update_crypto_keys(keys, c);
        finalbyte = c;
    } /* for */

    /* you have a 1/256 chance of passing this test incorrectly. :/ */
    if (finalbyte != verifier)
        BAIL_MACRO(PHYSFS_ERR_BAD_PASSWORD, 0);

    /* save the initial vector for seeking purposes. Not secure!! */
    memcpy(finfo->initial_crypto_keys, finfo->crypto_keys, 12);
    return 1;
} /* zip_prep_crypto_keys */


/*
 * Bridge physfs allocation functions to zlib's format...
 */
static voidpf zlibPhysfsAlloc(voidpf opaque, uInt items, uInt size)
{
    return ((PHYSFS_Allocator *) opaque)->Malloc(items * size);
} /* zlibPhysfsAlloc */

/*
 * Bridge physfs allocation functions to zlib's format...
 */
static void zlibPhysfsFree(voidpf opaque, voidpf address)
{
    ((PHYSFS_Allocator *) opaque)->Free(address);
} /* zlibPhysfsFree */


/*
 * Construct a new z_stream to a sane state.
 */
static void initializeZStream(z_stream *pstr)
{
    memset(pstr, '\0', sizeof (z_stream));
    pstr->zalloc = zlibPhysfsAlloc;
    pstr->zfree = zlibPhysfsFree;
    pstr->opaque = &allocator;
} /* initializeZStream */


static PHYSFS_ErrorCode zlib_error_code(int rc)
{
    switch (rc)
    {
        case Z_OK: return PHYSFS_ERR_OK;  /* not an error. */
        case Z_STREAM_END: return PHYSFS_ERR_OK; /* not an error. */
        case Z_ERRNO: return PHYSFS_ERR_IO;
        case Z_MEM_ERROR: return PHYSFS_ERR_OUT_OF_MEMORY;
        default: return PHYSFS_ERR_CORRUPT;
    } /* switch */
} /* zlib_error_string */


/*
 * Wrap all zlib calls in this, so the physfs error state is set appropriately.
 */
static int zlib_err(const int rc)
{
    PHYSFS_setErrorCode(zlib_error_code(rc));
    return rc;
} /* zlib_err */

/*
 * Hash a string for lookup an a ZIPinfo hashtable.
 */
static inline PHYSFS_uint32 zip_hash_string(const ZIPinfo *info, const char *s)
{
    return __PHYSFS_hashString(s, strlen(s)) % info->hashBuckets;
} /* zip_hash_string */

/*
 * Read an unsigned 64-bit int and swap to native byte order.
 */
static int readui64(PHYSFS_Io *io, PHYSFS_uint64 *val)
{
    PHYSFS_uint64 v;
    BAIL_IF_MACRO(!__PHYSFS_readAll(io, &v, sizeof (v)), ERRPASS, 0);
    *val = PHYSFS_swapULE64(v);
    return 1;
} /* readui64 */

/*
 * Read an unsigned 32-bit int and swap to native byte order.
 */
static int readui32(PHYSFS_Io *io, PHYSFS_uint32 *val)
{
    PHYSFS_uint32 v;
    BAIL_IF_MACRO(!__PHYSFS_readAll(io, &v, sizeof (v)), ERRPASS, 0);
    *val = PHYSFS_swapULE32(v);
    return 1;
} /* readui32 */


/*
 * Read an unsigned 16-bit int and swap to native byte order.
 */
static int readui16(PHYSFS_Io *io, PHYSFS_uint16 *val)
{
    PHYSFS_uint16 v;
    BAIL_IF_MACRO(!__PHYSFS_readAll(io, &v, sizeof (v)), ERRPASS, 0);
    *val = PHYSFS_swapULE16(v);
    return 1;
} /* readui16 */

  /*
  * Read an unsigned 8-bit int and swap to native byte order.
  */
static int readui8(PHYSFS_Io *io, PHYSFS_uint8 *val)
{
    PHYSFS_uint8 v;
    BAIL_IF_MACRO(!__PHYSFS_readAll(io, &v, sizeof(v)), ERRPASS, 0);
    *val = PHYSFS_swapULE8(v);
    return 1;
} /* readui8*/


static PHYSFS_sint64 ZIP_read(PHYSFS_Io *_io, void *buf, PHYSFS_uint64 len)
{
    ZIPfileinfo *finfo = (ZIPfileinfo *) _io->opaque;
    ZIPentry *entry = finfo->entry;
    PHYSFS_sint64 retval = 0;
    PHYSFS_sint64 maxread = (PHYSFS_sint64) len;
    PHYSFS_sint64 avail = entry->uncompressed_size -
                          finfo->uncompressed_position;

    if (avail < maxread)
        maxread = avail;

    BAIL_IF_MACRO(maxread == 0, ERRPASS, 0);    /* quick rejection. */

    if (entry->compression_method == COMPMETH_NONE)
        retval = zip_read_decrypt(finfo, buf, maxread);
    else
    {
        finfo->stream.next_out = buf;
        finfo->stream.avail_out = (uInt) maxread;

        while (retval < maxread)
        {
            PHYSFS_uint32 before = finfo->stream.total_out;
            int rc;

            if (finfo->stream.avail_in == 0)
            {
                PHYSFS_sint64 br;

                br = entry->compressed_size - finfo->compressed_position;
                if (br > 0)
                {
                    if (br > ZIP_READBUFSIZE)
                        br = ZIP_READBUFSIZE;

                    br = zip_read_decrypt(finfo, finfo->buffer, (PHYSFS_uint64) br);
                    if (br <= 0)
                        break;

                    finfo->compressed_position += (PHYSFS_uint32) br;
                    finfo->stream.next_in = finfo->buffer;
                    finfo->stream.avail_in = (PHYSFS_uint32) br;
                } /* if */
            } /* if */

            rc = zlib_err(inflate(&finfo->stream, Z_SYNC_FLUSH));
            retval += (finfo->stream.total_out - before);

            if (rc != Z_OK)
                break;
        } /* while */
    } /* else */

    if (retval > 0)
        finfo->uncompressed_position += (PHYSFS_uint32) retval;

    return retval;
} /* ZIP_read */


static PHYSFS_sint64 ZIP_write(PHYSFS_Io *io, const void *b, PHYSFS_uint64 len)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, -1);
} /* ZIP_write */


static PHYSFS_sint64 ZIP_tell(PHYSFS_Io *io)
{
    return ((ZIPfileinfo *) io->opaque)->uncompressed_position;
} /* ZIP_tell */


static int ZIP_seek(PHYSFS_Io *_io, PHYSFS_uint64 offset)
{
    ZIPfileinfo *finfo = (ZIPfileinfo *) _io->opaque;
    ZIPentry *entry = finfo->entry;
    PHYSFS_Io *io = finfo->io;
    const int encrypted = zip_entry_is_tradional_crypto(entry);

    BAIL_IF_MACRO(offset > entry->uncompressed_size, PHYSFS_ERR_PAST_EOF, 0);

    if (!encrypted && (entry->compression_method == COMPMETH_NONE))
    {
        PHYSFS_sint64 newpos = offset + entry->offset;
        BAIL_IF_MACRO(!io->seek(io, newpos), ERRPASS, 0);
        finfo->uncompressed_position = (PHYSFS_uint32) offset;
    } /* if */
    else if (ZIP_IS_AES(entry))
    {
        finfo->aes_ctx.encr_pos = AES_BLOCK_SIZE + 1;
        PHYSFS_sint64 newpos = offset + entry->offset;
        BAIL_IF_MACRO(!io->seek(io, newpos), ERRPASS, 0);
        finfo->uncompressed_position = (PHYSFS_uint32) offset;
    } /* else if */
    else
    {
        /*
         * If seeking backwards, we need to redecode the file
         *  from the start and throw away the compressed bits until we hit
         *  the offset we need. If seeking forward, we still need to
         *  decode, but we don't rewind first.
         */
        if (offset < finfo->uncompressed_position)
        {
            /* we do a copy so state is sane if inflateInit2() fails. */
            z_stream str;
            initializeZStream(&str);
            if (zlib_err(inflateInit2(&str, -MAX_WBITS)) != Z_OK)
                return 0;

            if (!io->seek(io, entry->offset + (encrypted ? 12 : 0)))
                return 0;

            inflateEnd(&finfo->stream);
            memcpy(&finfo->stream, &str, sizeof (z_stream));
            finfo->uncompressed_position = finfo->compressed_position = 0;

            if (encrypted)
                memcpy(finfo->crypto_keys, finfo->initial_crypto_keys, 12);
        } /* if */

        while (finfo->uncompressed_position != offset)
        {
            PHYSFS_uint8 buf[512];
            PHYSFS_uint32 maxread;

            maxread = (PHYSFS_uint32) (offset - finfo->uncompressed_position);
            if (maxread > sizeof (buf))
                maxread = sizeof (buf);

            if (ZIP_read(_io, buf, maxread) != maxread)
                return 0;
        } /* while */
    } /* else */

    return 1;
} /* ZIP_seek */


static PHYSFS_sint64 ZIP_length(PHYSFS_Io *io)
{
    const ZIPfileinfo *finfo = (ZIPfileinfo *) io->opaque;
    return (PHYSFS_sint64) finfo->entry->uncompressed_size;
} /* ZIP_length */


static PHYSFS_Io *zip_get_io(PHYSFS_Io *io, ZIPinfo *inf, ZIPentry *entry);

static PHYSFS_Io *ZIP_duplicate(PHYSFS_Io *io)
{
    ZIPfileinfo *origfinfo = (ZIPfileinfo *) io->opaque;
    PHYSFS_Io *retval = (PHYSFS_Io *) allocator.Malloc(sizeof (PHYSFS_Io));
    ZIPfileinfo *finfo = (ZIPfileinfo *) allocator.Malloc(sizeof (ZIPfileinfo));
    GOTO_IF_MACRO(!retval, PHYSFS_ERR_OUT_OF_MEMORY, failed);
    GOTO_IF_MACRO(!finfo, PHYSFS_ERR_OUT_OF_MEMORY, failed);
    memset(finfo, '\0', sizeof (*finfo));

    finfo->entry = origfinfo->entry;
    finfo->io = zip_get_io(origfinfo->io, NULL, finfo->entry);
    GOTO_IF_MACRO(!finfo->io, ERRPASS, failed);

    if (finfo->entry->compression_method != COMPMETH_NONE)
    {
        finfo->buffer = (PHYSFS_uint8 *) allocator.Malloc(ZIP_READBUFSIZE);
        GOTO_IF_MACRO(!finfo->buffer, PHYSFS_ERR_OUT_OF_MEMORY, failed);
        if (zlib_err(inflateInit2(&finfo->stream, -MAX_WBITS)) != Z_OK)
            goto failed;
    } /* if */

    memcpy(retval, io, sizeof (PHYSFS_Io));
    retval->opaque = finfo;
    return retval;

failed:
    if (finfo != NULL)
    {
        if (finfo->io != NULL)
            finfo->io->destroy(finfo->io);

        if (finfo->buffer != NULL)
        {
            allocator.Free(finfo->buffer);
            inflateEnd(&finfo->stream);
        } /* if */

        allocator.Free(finfo);
    } /* if */

    if (retval != NULL)
        allocator.Free(retval);

    return NULL;
} /* ZIP_duplicate */

static int ZIP_flush(PHYSFS_Io *io) { return 1;  /* no write support. */ }

static void ZIP_destroy(PHYSFS_Io *io)
{
    ZIPfileinfo *finfo = (ZIPfileinfo *) io->opaque;
    finfo->io->destroy(finfo->io);

    if (finfo->entry->compression_method != COMPMETH_NONE)
        inflateEnd(&finfo->stream);

    if (finfo->buffer != NULL)
        allocator.Free(finfo->buffer);

    allocator.Free(finfo);
    allocator.Free(io);
} /* ZIP_destroy */


static const PHYSFS_Io ZIP_Io =
{
    CURRENT_PHYSFS_IO_API_VERSION, NULL,
    ZIP_read,
    ZIP_write,
    ZIP_seek,
    ZIP_tell,
    ZIP_length,
    ZIP_duplicate,
    ZIP_flush,
    ZIP_destroy
};



static PHYSFS_sint64 zip_find_end_of_central_dir(PHYSFS_Io *io, PHYSFS_sint64 *len)
{
    PHYSFS_uint8 buf[256];
    PHYSFS_uint8 extra[4] = { 0, 0, 0, 0 };
    PHYSFS_sint32 i = 0;
    PHYSFS_sint64 filelen;
    PHYSFS_sint64 filepos;
    PHYSFS_sint32 maxread;
    PHYSFS_sint32 totalread = 0;
    int found = 0;

    filelen = io->length(io);
    BAIL_IF_MACRO(filelen == -1, ERRPASS, -1);

    /*
     * Jump to the end of the file and start reading backwards.
     *  The last thing in the file is the zipfile comment, which is variable
     *  length, and the field that specifies its size is before it in the
     *  file (argh!)...this means that we need to scan backwards until we
     *  hit the end-of-central-dir signature. We can then sanity check that
     *  the comment was as big as it should be to make sure we're in the
     *  right place. The comment length field is 16 bits, so we can stop
     *  searching for that signature after a little more than 64k at most,
     *  and call it a corrupted zipfile.
     */

    if (sizeof (buf) < filelen)
    {
        filepos = filelen - sizeof (buf);
        maxread = sizeof (buf);
    } /* if */
    else
    {
        filepos = 0;
        maxread = (PHYSFS_uint32) filelen;
    } /* else */

    while ((totalread < filelen) && (totalread < 65557))
    {
        BAIL_IF_MACRO(!io->seek(io, filepos), ERRPASS, -1);

        /* make sure we catch a signature between buffers. */
        if (totalread != 0)
        {
            if (!__PHYSFS_readAll(io, buf, maxread - 4))
                return -1;
            memcpy(&buf[maxread - 4], &extra, sizeof (extra));
            totalread += maxread - 4;
        } /* if */
        else
        {
            if (!__PHYSFS_readAll(io, buf, maxread))
                return -1;
            totalread += maxread;
        } /* else */

        memcpy(&extra, buf, sizeof (extra));

        for (i = maxread - 4; i > 0; i--)
        {
            if ((buf[i + 0] == 0x50) &&
                (buf[i + 1] == 0x4B) &&
                (buf[i + 2] == 0x05) &&
                (buf[i + 3] == 0x06) )
            {
                found = 1;  /* that's the signature! */
                break;  
            } /* if */
        } /* for */

        if (found)
            break;

        filepos -= (maxread - 4);
        if (filepos < 0)
            filepos = 0;
    } /* while */

    BAIL_IF_MACRO(!found, PHYSFS_ERR_UNSUPPORTED, -1);

    if (len != NULL)
        *len = filelen;

    return (filepos + i);
} /* zip_find_end_of_central_dir */


static int isZip(PHYSFS_Io *io)
{
    PHYSFS_uint32 sig = 0;
    int retval = 0;

    /*
     * The first thing in a zip file might be the signature of the
     *  first local file record, so it makes for a quick determination.
     */
    if (readui32(io, &sig))
    {
        retval = (sig == ZIP_LOCAL_FILE_SIG);
        if (!retval)
        {
            /*
             * No sig...might be a ZIP with data at the start
             *  (a self-extracting executable, etc), so we'll have to do
             *  it the hard way...
             */
            retval = (zip_find_end_of_central_dir(io, NULL) != -1);
        } /* if */
    } /* if */

    return retval;
} /* isZip */


/* Find the ZIPentry for a path in platform-independent notation. */
static ZIPentry *zip_find_entry(ZIPinfo *info, const char *path)
{
    PHYSFS_uint32 hashval;
    ZIPentry *prev = NULL;
    ZIPentry *retval;

    if (*path == '\0')
        return &info->root;

    hashval = zip_hash_string(info, path);
    for (retval = info->hash[hashval]; retval; retval = retval->hashnext)
    {
        if (__PHYSFS_utf8stricmp(retval->name, path) == 0)
        {
            if (prev != NULL)  /* move this to the front of the list */
            {
                prev->hashnext = retval->hashnext;
                retval->hashnext = info->hash[hashval];
                info->hash[hashval] = retval;
            } /* if */

            return retval;
        } /* if */

        prev = retval;
    } /* for */

    BAIL_MACRO(PHYSFS_ERR_NOT_FOUND, NULL);
} /* zip_find_entry */


/* Convert paths from old, buggy DOS zippers... */
static void zip_convert_dos_path(ZIPentry *entry, char *path)
{
    PHYSFS_uint8 hosttype = (PHYSFS_uint8) ((entry->version >> 8) & 0xFF);
    if (hosttype == 0)  /* FS_FAT_ */
    {
        while (*path)
        {
            if (*path == '\\')
                *path = '/';
            path++;
        } /* while */
    } /* if */
} /* zip_convert_dos_path */


static void zip_expand_symlink_path(char *path)
{
    char *ptr = path;
    char *prevptr = path;

    while (1)
    {
        ptr = strchr(ptr, '/');
        if (ptr == NULL)
            break;

        if (*(ptr + 1) == '.')
        {
            if (*(ptr + 2) == '/')
            {
                /* current dir in middle of string: ditch it. */
                memmove(ptr, ptr + 2, strlen(ptr + 2) + 1);
            } /* else if */

            else if (*(ptr + 2) == '\0')
            {
                /* current dir at end of string: ditch it. */
                *ptr = '\0';
            } /* else if */

            else if (*(ptr + 2) == '.')
            {
                if (*(ptr + 3) == '/')
                {
                    /* parent dir in middle: move back one, if possible. */
                    memmove(prevptr, ptr + 4, strlen(ptr + 4) + 1);
                    ptr = prevptr;
                    while (prevptr != path)
                    {
                        prevptr--;
                        if (*prevptr == '/')
                        {
                            prevptr++;
                            break;
                        } /* if */
                    } /* while */
                } /* if */

                if (*(ptr + 3) == '\0')
                {
                    /* parent dir at end: move back one, if possible. */
                    *prevptr = '\0';
                } /* if */
            } /* if */
        } /* if */
        else
        {
            prevptr = ptr;
            ptr++;
        } /* else */
    } /* while */
} /* zip_expand_symlink_path */

/* (forward reference: zip_follow_symlink and zip_resolve call each other.) */
static int zip_resolve(PHYSFS_Io *io, ZIPinfo *info, ZIPentry *entry);

/*
 * Look for the entry named by (path). If it exists, resolve it, and return
 *  a pointer to that entry. If it's another symlink, keep resolving until you
 *  hit a real file and then return a pointer to the final non-symlink entry.
 *  If there's a problem, return NULL.
 */
static ZIPentry *zip_follow_symlink(PHYSFS_Io *io, ZIPinfo *info, char *path)
{
    ZIPentry *entry;

    zip_expand_symlink_path(path);
    entry = zip_find_entry(info, path);
    if (entry != NULL)
    {
        if (!zip_resolve(io, info, entry))  /* recursive! */
            entry = NULL;
        else
        {
            if (entry->symlink != NULL)
                entry = entry->symlink;
        } /* else */
    } /* if */

    return entry;
} /* zip_follow_symlink */


static int zip_resolve_symlink(PHYSFS_Io *io, ZIPinfo *info, ZIPentry *entry)
{
    const PHYSFS_uint64 size = entry->uncompressed_size;
    char *path = NULL;
    int rc = 0;

    /*
     * We've already parsed the local file header of the symlink at this
     *  point. Now we need to read the actual link from the file data and
     *  follow it.
     */

    BAIL_IF_MACRO(!io->seek(io, entry->offset), ERRPASS, 0);

    path = (char *) __PHYSFS_smallAlloc(size + 1);
    BAIL_IF_MACRO(!path, PHYSFS_ERR_OUT_OF_MEMORY, 0);
    
    if (entry->compression_method == COMPMETH_NONE)
        rc = __PHYSFS_readAll(io, path, size);

    else  /* symlink target path is compressed... */
    {
        z_stream stream;
        const PHYSFS_uint64 complen = entry->compressed_size;
        PHYSFS_uint8 *compressed = (PHYSFS_uint8*) __PHYSFS_smallAlloc(complen);
        if (compressed != NULL)
        {
            if (__PHYSFS_readAll(io, compressed, complen))
            {
                initializeZStream(&stream);
                stream.next_in = compressed;
                stream.avail_in = complen;
                stream.next_out = (unsigned char *) path;
                stream.avail_out = size;
                if (zlib_err(inflateInit2(&stream, -MAX_WBITS)) == Z_OK)
                {
                    rc = zlib_err(inflate(&stream, Z_FINISH));
                    inflateEnd(&stream);

                    /* both are acceptable outcomes... */
                    rc = ((rc == Z_OK) || (rc == Z_STREAM_END));
                } /* if */
            } /* if */
            __PHYSFS_smallFree(compressed);
        } /* if */
    } /* else */

    if (rc)
    {
        path[entry->uncompressed_size] = '\0';    /* null-terminate it. */
        zip_convert_dos_path(entry, path);
        entry->symlink = zip_follow_symlink(io, info, path);
    } /* else */

    __PHYSFS_smallFree(path);

    return (entry->symlink != NULL);
} /* zip_resolve_symlink */


/*
 * Parse the local file header of an entry, and update entry->offset.
 */
static int zip_parse_local(PHYSFS_Io *io, ZIPentry *entry)
{
    PHYSFS_uint32 ui32;
    PHYSFS_uint16 ui16;
    PHYSFS_uint16 fnamelen;
    PHYSFS_uint16 extralen;
    PHYSFS_uint8 ui8;

    /*
     * crc and (un)compressed_size are always zero if this is a "JAR"
     *  archive created with Sun's Java tools, apparently. We only
     *  consider this archive corrupted if those entries don't match and
     *  aren't zero. That seems to work well.
     * We also ignore a mismatch if the value is 0xFFFFFFFF here, since it's
     *  possible that's a Zip64 thing.
     */

    /* !!! FIXME: apparently these are zero if general purpose bit 3 is set,
       !!! FIXME:  which is probably true for Jar files, fwiw, but we don't
       !!! FIXME:  care about these values anyhow. */

    BAIL_IF_MACRO(!io->seek(io, entry->offset), ERRPASS, 0);
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 != ZIP_LOCAL_FILE_SIG, PHYSFS_ERR_CORRUPT, 0);
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);
    BAIL_IF_MACRO(ui16 != entry->version_needed, PHYSFS_ERR_CORRUPT, 0);
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);  /* general bits. */
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);
    BAIL_IF_MACRO(ZIP_IS_AES(entry) && !ui16 != entry->compression_method, PHYSFS_ERR_CORRUPT, 0);
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);  /* date/time */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 && (ui32 != entry->crc), PHYSFS_ERR_CORRUPT, 0);

    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 && (ui32 != 0xFFFFFFFF) &&
                  (ui32 != entry->compressed_size), PHYSFS_ERR_CORRUPT, 0);

    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 && (ui32 != 0xFFFFFFFF) &&
                 (ui32 != entry->uncompressed_size), PHYSFS_ERR_CORRUPT, 0);

    BAIL_IF_MACRO(!readui16(io, &fnamelen), ERRPASS, 0);
    BAIL_IF_MACRO(!readui16(io, &extralen), ERRPASS, 0);

    entry->offset += fnamelen + extralen + 30;

    if (ZIP_IS_AES(entry)) {
        int i;
        BAIL_IF_MACRO(COMPMETH_NONE != entry->compression_method, PHYSFS_ERR_CORRUPT, 0);
        BAIL_IF_MACRO(!io->seek(io, entry->offset), PHYSFS_ERR_CORRUPT, 0);

        /* Read Salt value (8, 12 or 16 bytes) */
        if (entry->aes_data.key_strength == ZIP_AES_128_BITS) {
            for (i = 0; i < 8; i++)
            {
                BAIL_IF_MACRO(!readui8(io, &ui8), PHYSFS_ERR_CORRUPT, 0);
                entry->aes_data.salt[i] = ui8;
            }
            entry->offset += 8;
        }
        if (entry->aes_data.key_strength == ZIP_AES_192_BITS) {
            for (i = 0; i < 12; i++)
            {
                BAIL_IF_MACRO(!readui8(io, &ui8), PHYSFS_ERR_CORRUPT, 0);
                entry->aes_data.salt[i] = ui8;
            }
            entry->offset += 12;
        }
        if (entry->aes_data.key_strength == ZIP_AES_256_BITS) {
            for (i = 0; i < 16; i++)
            {
                BAIL_IF_MACRO(!readui8(io, &ui8), PHYSFS_ERR_CORRUPT, 0);
                entry->aes_data.salt[i] = ui8;
            }
            entry->offset += 16;
        }

        BAIL_IF_MACRO(!readui16(io, &entry->aes_data.pass_verification), PHYSFS_ERR_CORRUPT, 0);
        entry->offset += 2;
        /* FIXME save auth code and check integrity */
        /* Lets ignore the CRC and Auth code for simplicity */
    }

    return 1;
} /* zip_parse_local */


static int zip_resolve(PHYSFS_Io *io, ZIPinfo *info, ZIPentry *entry)
{
    int retval = 1;
    const ZipResolveType resolve_type = entry->resolved;

    if (resolve_type == ZIP_DIRECTORY)
        return 1;   /* we're good. */

    /* Don't bother if we've failed to resolve this entry before. */
    BAIL_IF_MACRO(resolve_type == ZIP_BROKEN_FILE, PHYSFS_ERR_CORRUPT, 0);
    BAIL_IF_MACRO(resolve_type == ZIP_BROKEN_SYMLINK, PHYSFS_ERR_CORRUPT, 0);

    /* uhoh...infinite symlink loop! */
    BAIL_IF_MACRO(resolve_type == ZIP_RESOLVING, PHYSFS_ERR_SYMLINK_LOOP, 0);

    /*
     * We fix up the offset to point to the actual data on the
     *  first open, since we don't want to seek across the whole file on
     *  archive open (can be SLOW on large, CD-stored files), but we
     *  need to check the local file header...not just for corruption,
     *  but since it stores offset info the central directory does not.
     */
    if (resolve_type != ZIP_RESOLVED)
    {
        entry->resolved = ZIP_RESOLVING;

        retval = zip_parse_local(io, entry);
        if (retval)
        {
            /*
             * If it's a symlink, find the original file. This will cause
             *  resolution of other entries (other symlinks and, eventually,
             *  the real file) if all goes well.
             */
            if (resolve_type == ZIP_UNRESOLVED_SYMLINK)
                retval = zip_resolve_symlink(io, info, entry);
        } /* if */

        if (resolve_type == ZIP_UNRESOLVED_SYMLINK)
            entry->resolved = ((retval) ? ZIP_RESOLVED : ZIP_BROKEN_SYMLINK);
        else if (resolve_type == ZIP_UNRESOLVED_FILE)
            entry->resolved = ((retval) ? ZIP_RESOLVED : ZIP_BROKEN_FILE);
    } /* if */

    return retval;
} /* zip_resolve */


static int zip_hash_entry(ZIPinfo *info, ZIPentry *entry);

/* Fill in missing parent directories. */
static ZIPentry *zip_hash_ancestors(ZIPinfo *info, char *name)
{
    ZIPentry *retval = &info->root;
    char *sep = strrchr(name, '/');

    if (sep)
    {
        const size_t namelen = (sep - name) +1;

        *sep = '\0';  /* chop off last piece. */
        retval = zip_find_entry(info, name);
        *sep = '/';

        if (retval != NULL)
        {
            if (retval->resolved != ZIP_DIRECTORY)
                BAIL_MACRO(PHYSFS_ERR_CORRUPT, NULL);
            return retval;  /* already hashed. */
        } /* if */

        /* okay, this is a new dir. Build and hash us. */
        retval = (ZIPentry *) allocator.Malloc(sizeof (ZIPentry) + namelen);
        BAIL_IF_MACRO(!retval, PHYSFS_ERR_OUT_OF_MEMORY, NULL);
        memset(retval, '\0', sizeof (*retval));
        retval->name = ((char *) retval) + sizeof (ZIPentry);
        memcpy(retval->name, name, namelen);
        retval->name[namelen - 1] = '\0';
        retval->resolved = ZIP_DIRECTORY;
        if (!zip_hash_entry(info, retval))
        {
            allocator.Free(retval);
            return NULL;
        } /* if */
    } /* else */

    return retval;
} /* zip_hash_ancestors */


static int zip_hash_entry(ZIPinfo *info, ZIPentry *entry)
{
    PHYSFS_uint32 hashval;
    ZIPentry *parent;

    assert(!zip_find_entry(info, entry->name));  /* checked elsewhere */

    parent = zip_hash_ancestors(info, entry->name);
    if (!parent)
        return 0;

    size_t len = strlen(entry->name);
    if (entry->name[len -1] == '/') {

        int i = 0;
    }


    hashval = zip_hash_string(info, entry->name);
    entry->hashnext = info->hash[hashval];
    info->hash[hashval] = entry;

    entry->sibling = parent->children;
    parent->children = entry;
    return 1;
} /* zip_hash_entry */


static int zip_entry_is_symlink(const ZIPentry *entry)
{
    return ((entry->resolved == ZIP_UNRESOLVED_SYMLINK) ||
            (entry->resolved == ZIP_BROKEN_SYMLINK) ||
            (entry->symlink));
} /* zip_entry_is_symlink */


static int zip_version_does_symlinks(PHYSFS_uint32 version)
{
    int retval = 0;
    PHYSFS_uint8 hosttype = (PHYSFS_uint8) ((version >> 8) & 0xFF);

    switch (hosttype)
    {
            /*
             * These are the platforms that can NOT build an archive with
             *  symlinks, according to the Info-ZIP project.
             */
        case 0:  /* FS_FAT_  */
        case 1:  /* AMIGA_   */
        case 2:  /* VMS_     */
        case 4:  /* VM_CSM_  */
        case 6:  /* FS_HPFS_ */
        case 11: /* FS_NTFS_ */
        case 14: /* FS_VFAT_ */
        case 13: /* ACORN_   */
        case 15: /* MVS_     */
        case 18: /* THEOS_   */
            break;  /* do nothing. */

        default:  /* assume the rest to be unix-like. */
            retval = 1;
            break;
    } /* switch */

    return retval;
} /* zip_version_does_symlinks */


static int zip_has_symlink_attr(ZIPentry *entry, PHYSFS_uint32 extern_attr)
{
    PHYSFS_uint16 xattr = ((extern_attr >> 16) & 0xFFFF);
    return ( (zip_version_does_symlinks(entry->version)) &&
             (entry->uncompressed_size > 0) &&
             ((xattr & UNIX_FILETYPE_MASK) == UNIX_FILETYPE_SYMLINK) );
} /* zip_has_symlink_attr */


static PHYSFS_sint64 zip_dos_time_to_physfs_time(PHYSFS_uint32 dostime)
{
    PHYSFS_uint32 dosdate;
    struct tm unixtime;
    memset(&unixtime, '\0', sizeof (unixtime));

    dosdate = (PHYSFS_uint32) ((dostime >> 16) & 0xFFFF);
    dostime &= 0xFFFF;

    /* dissect date */
    unixtime.tm_year = ((dosdate >> 9) & 0x7F) + 80;
    unixtime.tm_mon  = ((dosdate >> 5) & 0x0F) - 1;
    unixtime.tm_mday = ((dosdate     ) & 0x1F);

    /* dissect time */
    unixtime.tm_hour = ((dostime >> 11) & 0x1F);
    unixtime.tm_min  = ((dostime >>  5) & 0x3F);
    unixtime.tm_sec  = ((dostime <<  1) & 0x3E);

    /* let mktime calculate daylight savings time. */
    unixtime.tm_isdst = -1;

    return ((PHYSFS_sint64) mktime(&unixtime));
} /* zip_dos_time_to_physfs_time */


static ZIPentry *zip_load_entry(PHYSFS_Io *io, const int zip64,
                                const PHYSFS_uint64 ofs_fixup)
{
    ZIPentry entry;
    ZIPentry *retval = NULL;
    PHYSFS_uint16 fnamelen, extralen, commentlen;
    PHYSFS_uint32 external_attr;
    PHYSFS_uint32 starting_disk;
    PHYSFS_uint64 offset;
    PHYSFS_uint16 ui16;
    PHYSFS_uint32 ui32;
    PHYSFS_sint64 si64;
    //PHYSFS_uint32 entrySize;

    memset(&entry, '\0', sizeof (entry));

    /* sanity check with central directory signature... */
    if (!readui32(io, &ui32)) return NULL;
    BAIL_IF_MACRO(ui32 != ZIP_CENTRAL_DIR_SIG, PHYSFS_ERR_CORRUPT, NULL);

    /* Get the pertinent parts of the record... */
    if (!readui16(io, &entry.version)) return NULL;
    if (!readui16(io, &entry.version_needed)) return NULL;
    if (!readui16(io, &entry.general_bits)) return NULL;  /* general bits */
    if (!readui16(io, &entry.compression_method)) return NULL;
    if (!readui32(io, &entry.dos_mod_time)) return NULL;
    entry.last_mod_time = zip_dos_time_to_physfs_time(entry.dos_mod_time);
    if (!readui32(io, &entry.crc)) return NULL;
    if (!readui32(io, &ui32)) return NULL;
    entry.compressed_size = (PHYSFS_uint64) ui32;
    if (!readui32(io, &ui32)) return NULL;
    entry.uncompressed_size = (PHYSFS_uint64) ui32;
    if (!readui16(io, &fnamelen)) return NULL;
    if (!readui16(io, &extralen)) return NULL;
    if (!readui16(io, &commentlen)) return NULL;
    if (!readui16(io, &ui16)) return NULL;
    starting_disk = (PHYSFS_uint32) ui16;
    if (!readui16(io, &ui16)) return NULL;  /* internal file attribs */
    if (!readui32(io, &external_attr)) return NULL;
    if (!readui32(io, &ui32)) return NULL;
    offset = (PHYSFS_uint64) ui32;

    retval = (ZIPentry *) allocator.Malloc(sizeof (ZIPentry) + fnamelen + 1);
    BAIL_IF_MACRO(retval == NULL, PHYSFS_ERR_OUT_OF_MEMORY, 0);
    memcpy(retval, &entry, sizeof (*retval));
    retval->name = ((char *) retval) + sizeof (ZIPentry);

    if (!__PHYSFS_readAll(io, retval->name, fnamelen))
        goto zip_load_entry_puked;

    retval->name[fnamelen] = '\0';  /* null-terminate the filename. */
    zip_convert_dos_path(retval, retval->name);

    retval->symlink = NULL;  /* will be resolved later, if necessary. */

    if (retval->name[fnamelen - 1] == '/')
    {
        retval->name[fnamelen - 1] = '\0';
        retval->resolved = ZIP_DIRECTORY;
    } /* if */
    else
    {
        retval->resolved = (zip_has_symlink_attr(&entry, external_attr)) ?
                                ZIP_UNRESOLVED_SYMLINK : ZIP_UNRESOLVED_FILE;
    } /* else */

    si64 = io->tell(io);
    if (si64 == -1)
        goto zip_load_entry_puked;

    /*
     * The actual sizes didn't fit in 32-bits; look for the Zip64
     *  extended information extra field...
     */
    {
        int found = 0;
        PHYSFS_uint16 sig, len;
        PHYSFS_uint16 extralen2 = extralen;
        PHYSFS_sint64 si64_2;
        while (extralen2 > 4)
        {
            if (!readui16(io, &sig))
                goto zip_load_entry_puked;
            else if (!readui16(io, &len))
                goto zip_load_entry_puked;

            si64_2 = io->tell(io) + len;
            extralen2 -= 4 + len;
            if (sig == ZIP64_EXTENDED_INFO_EXTRA_FIELD_SIG)
            {
                if (retval->uncompressed_size == 0xFFFFFFFF)
                {
                    GOTO_IF_MACRO(len < 8, PHYSFS_ERR_CORRUPT, zip_load_entry_puked);
                    if (!readui64(io, &retval->uncompressed_size))
                        goto zip_load_entry_puked;
                    len -= 8;
                } /* if */

                if (retval->compressed_size == 0xFFFFFFFF)
                {
                    GOTO_IF_MACRO(len < 8, PHYSFS_ERR_CORRUPT, zip_load_entry_puked);
                    if (!readui64(io, &retval->compressed_size))
                        goto zip_load_entry_puked;
                    len -= 8;
                } /* if */

                if (offset == 0xFFFFFFFF)
                {
                    GOTO_IF_MACRO(len < 8, PHYSFS_ERR_CORRUPT, zip_load_entry_puked);
                    if (!readui64(io, &offset))
                        goto zip_load_entry_puked;
                    len -= 8;
                } /* if */

                if (starting_disk == 0xFFFFFFFF)
                {
                    GOTO_IF_MACRO(len < 8, PHYSFS_ERR_CORRUPT, zip_load_entry_puked);
                    if (!readui32(io, &starting_disk))
                        goto zip_load_entry_puked;
                    len -= 4;
                } /* if */

                GOTO_IF_MACRO(len != 0, PHYSFS_ERR_CORRUPT, zip_load_entry_puked);
            } /* if */
            else if (sig == ZIP_AES_HEADER_EXTRA_FIELD_SIG && retval->compression_method == COMPMETH_AES) {
                PHYSFS_uint16 zip_vendor; // extra_header
                BAIL_IF_MACRO(!readui16(io, &zip_vendor), PHYSFS_ERR_CORRUPT, 0);

                BAIL_IF_MACRO(((zip_vendor != ZIP_AE1_VENDOR_VERSION) && (zip_vendor != ZIP_AE2_VENDOR_VERSION)), PHYSFS_ERR_CORRUPT, 0);
                BAIL_IF_MACRO(!readui16(io, &zip_vendor), PHYSFS_ERR_CORRUPT, 0); /* 'AE' */
                BAIL_IF_MACRO(zip_vendor != ZIP_AES_VENDOR_ID, PHYSFS_ERR_CORRUPT, 0);
                BAIL_IF_MACRO(!readui8(io, &retval->aes_data.key_strength), PHYSFS_ERR_CORRUPT, 0);		/* Key Strength */
                BAIL_IF_MACRO(!readui16(io, &retval->aes_data.compression), PHYSFS_ERR_CORRUPT, 0);  /* Compression method */
                BAIL_IF_MACRO(retval->aes_data.compression != 0, PHYSFS_ERR_CORRUPT, 0); /* Not supported compression */
                retval->compression_method = COMPMETH_NONE;
            }
            else {
                if (!io->seek(io, si64_2))
                    goto zip_load_entry_puked;
            }
            
            //break;
        } /* while */

        
    } /* if */

    GOTO_IF_MACRO(starting_disk != 0, PHYSFS_ERR_CORRUPT, zip_load_entry_puked);

    retval->offset = offset + ofs_fixup;

    /* seek to the start of the next entry in the central directory... */
    if (!io->seek(io, si64 + extralen + commentlen))
        goto zip_load_entry_puked;

    return retval;  /* success. */

zip_load_entry_puked:
    allocator.Free(retval);
    return NULL;  /* failure. */
} /* zip_load_entry */


/* This leaves things allocated on error; the caller will clean up the mess. */
static int zip_load_entries(ZIPinfo *info,
                            const PHYSFS_uint64 data_ofs,
                            const PHYSFS_uint64 central_ofs,
                            const PHYSFS_uint64 entry_count)
{
    PHYSFS_Io *io = info->io;
    const int zip64 = info->zip64;
    PHYSFS_uint64 i;

    if (!io->seek(io, central_ofs))
        return 0;

    for (i = 0; i < entry_count; i++)
    {
        ZIPentry *entry = zip_load_entry(io, zip64, data_ofs);
        ZIPentry *find;

        if (!entry)
            return 0;

        find = zip_find_entry(info, entry->name);
        if (find != NULL)  /* duplicate? */
        {
            if (find->last_mod_time != 0)  /* duplicate? */
            {
                allocator.Free(entry);
                BAIL_MACRO(PHYSFS_ERR_CORRUPT, 0);
            } /* if */
            else  /* we filled this in as a placeholder. Update it. */
            {
                find->offset = entry->offset;
                find->version = entry->version;
                find->version_needed = entry->version_needed;
                find->compression_method = entry->compression_method;
                find->crc = entry->crc;
                find->compressed_size = entry->compressed_size;
                find->uncompressed_size = entry->uncompressed_size;
                find->last_mod_time = entry->last_mod_time;
                allocator.Free(entry);
                continue;
            } /* else */
        } /* if */

        if (!zip_hash_entry(info, entry))
        {
            allocator.Free(entry);
            return 0;
        } /* if */

        if (zip_entry_is_tradional_crypto(entry))
            info->has_crypto = 1;
    } /* for */

    return 1;
} /* zip_load_entries */


static PHYSFS_sint64 zip64_find_end_of_central_dir(PHYSFS_Io *io,
                                                   PHYSFS_sint64 _pos,
                                                   PHYSFS_uint64 offset)
{
    /*
     * Naturally, the offset is useless to us; it is the offset from the
     *  start of file, which is meaningless if we've appended this .zip to
     *  a self-extracting .exe. We need to find this on our own. It should
     *  be directly before the locator record, but the record in question,
     *  like the original end-of-central-directory record, ends with a
     *  variable-length field. Unlike the original, which has to store the
     *  size of that variable-length field in a 16-bit int and thus has to be
     *  within 64k, the new one gets 64-bits.
     *
     * Fortunately, the only currently-specified record for that variable
     *  length block is some weird proprietary thing that deals with EBCDIC
     *  and tape backups or something. So we don't seek far.
     */

    PHYSFS_uint32 ui32;
    const PHYSFS_uint64 pos = (PHYSFS_uint64) _pos;

    assert(_pos > 0);

    /* Try offset specified in the Zip64 end of central directory locator. */
    /* This works if the entire PHYSFS_Io is the zip file. */
    BAIL_IF_MACRO(!io->seek(io, offset), ERRPASS, -1);
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, -1);
    if (ui32 == ZIP64_END_OF_CENTRAL_DIR_SIG)
        return offset;

    /* Try 56 bytes before the Zip64 end of central directory locator. */
    /* This works if the record isn't variable length and is version 1. */
    if (pos > 56)
    {
        BAIL_IF_MACRO(!io->seek(io, pos-56), ERRPASS, -1);
        BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, -1);
        if (ui32 == ZIP64_END_OF_CENTRAL_DIR_SIG)
            return pos-56;
    } /* if */

    /* Try 84 bytes before the Zip64 end of central directory locator. */
    /* This works if the record isn't variable length and is version 2. */
    if (pos > 84)
    {
        BAIL_IF_MACRO(!io->seek(io, pos-84), ERRPASS, -1);
        BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, -1);
        if (ui32 == ZIP64_END_OF_CENTRAL_DIR_SIG)
            return pos-84;
    } /* if */

    /* Ok, brute force: we know it's between (offset) and (pos) somewhere. */
    /*  Just try moving back at most 256k. Oh well. */
    if ((offset < pos) && (pos > 4))
    {
        const PHYSFS_uint64 maxbuflen = 256 * 1024;
        PHYSFS_uint64 len = pos - offset;
        PHYSFS_uint8 *buf = NULL;
        PHYSFS_sint32 i;

        if (len > maxbuflen)
            len = maxbuflen;

        buf = (PHYSFS_uint8 *) __PHYSFS_smallAlloc(len);
        BAIL_IF_MACRO(!buf, PHYSFS_ERR_OUT_OF_MEMORY, -1);

        if (!io->seek(io, pos - len) || !__PHYSFS_readAll(io, buf, len))
        {
            __PHYSFS_smallFree(buf);
            return -1;  /* error was set elsewhere. */
        } /* if */

        for (i = (PHYSFS_sint32) (len - 4); i >= 0; i--)
        {
            if ( (buf[i] == 0x50) && (buf[i+1] == 0x4b) &&
                 (buf[i+2] == 0x06) && (buf[i+3] == 0x06) )
            {
                __PHYSFS_smallFree(buf);
                return pos - (len - i);
            } /* if */
        } /* for */

        __PHYSFS_smallFree(buf);
    } /* if */

    BAIL_MACRO(PHYSFS_ERR_CORRUPT, -1);  /* didn't find it. */
} /* zip64_find_end_of_central_dir */


static int zip64_parse_end_of_central_dir(ZIPinfo *info,
                                          PHYSFS_uint64 *data_start,
                                          PHYSFS_uint64 *dir_ofs,
                                          PHYSFS_uint64 *entry_count,
                                          PHYSFS_sint64 pos)
{
    PHYSFS_Io *io = info->io;
    PHYSFS_uint64 ui64;
    PHYSFS_uint32 ui32;
    PHYSFS_uint16 ui16;

    /* We should be positioned right past the locator signature. */

    if ((pos < 0) || (!io->seek(io, pos)))
        return 0;

    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    if (ui32 != ZIP64_END_OF_CENTRAL_DIRECTORY_LOCATOR_SIG)
        return -1;  /* it's not a Zip64 archive. Not an error, though! */

    info->zip64 = 1;

    /* number of the disk with the start of the central directory. */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 != 0, PHYSFS_ERR_CORRUPT, 0);

    /* offset of Zip64 end of central directory record. */
    BAIL_IF_MACRO(!readui64(io, &ui64), ERRPASS, 0);

    /* total number of disks */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 != 1, PHYSFS_ERR_CORRUPT, 0);

    pos = zip64_find_end_of_central_dir(io, pos, ui64);
    if (pos < 0)
        return 0;  /* oh well. */

    /*
     * For self-extracting archives, etc, there's crapola in the file
     *  before the zipfile records; we calculate how much data there is
     *  prepended by determining how far the zip64-end-of-central-directory
     *  offset is from where it is supposed to be...the difference in bytes
     *  is how much arbitrary data is at the start of the physical file.
     */
    assert(((PHYSFS_uint64) pos) >= ui64);
    *data_start = ((PHYSFS_uint64) pos) - ui64;

    BAIL_IF_MACRO(!io->seek(io, pos), ERRPASS, 0);

    /* check signature again, just in case. */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 != ZIP64_END_OF_CENTRAL_DIR_SIG, PHYSFS_ERR_CORRUPT, 0);

    /* size of Zip64 end of central directory record. */
    BAIL_IF_MACRO(!readui64(io, &ui64), ERRPASS, 0);

    /* version made by. */
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);

    /* version needed to extract. */
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);

    /* number of this disk. */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 != 0, PHYSFS_ERR_CORRUPT, 0);

    /* number of disk with start of central directory record. */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 != 0, PHYSFS_ERR_CORRUPT, 0);

    /* total number of entries in the central dir on this disk */
    BAIL_IF_MACRO(!readui64(io, &ui64), ERRPASS, 0);

    /* total number of entries in the central dir */
    BAIL_IF_MACRO(!readui64(io, entry_count), ERRPASS, 0);
    BAIL_IF_MACRO(ui64 != *entry_count, PHYSFS_ERR_CORRUPT, 0);

    /* size of the central directory */
    BAIL_IF_MACRO(!readui64(io, &ui64), ERRPASS, 0);

    /* offset of central directory */
    BAIL_IF_MACRO(!readui64(io, dir_ofs), ERRPASS, 0);

    /* Since we know the difference, fix up the central dir offset... */
    *dir_ofs += *data_start;

    /*
     * There are more fields here, for encryption and feature-specific things,
     *  but we don't care about any of them at the moment.
     */

    return 1;  /* made it. */
} /* zip64_parse_end_of_central_dir */


static int zip_parse_end_of_central_dir(ZIPinfo *info,
                                        PHYSFS_uint64 *data_start,
                                        PHYSFS_uint64 *dir_ofs,
                                        PHYSFS_uint64 *entry_count)
{
    PHYSFS_Io *io = info->io;
    PHYSFS_uint16 entryCount16;
    PHYSFS_uint32 offset32;
    PHYSFS_uint32 ui32;
    PHYSFS_uint16 ui16;
    PHYSFS_sint64 len;
    PHYSFS_sint64 pos;
    int rc;

    /* find the end-of-central-dir record, and seek to it. */
    pos = zip_find_end_of_central_dir(io, &len);
    BAIL_IF_MACRO(pos == -1, ERRPASS, 0);
    BAIL_IF_MACRO(!io->seek(io, pos), ERRPASS, 0);

    /* check signature again, just in case. */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);
    BAIL_IF_MACRO(ui32 != ZIP_END_OF_CENTRAL_DIR_SIG, PHYSFS_ERR_CORRUPT, 0);

    /* Seek back to see if "Zip64 end of central directory locator" exists. */
    /* this record is 20 bytes before end-of-central-dir */
    rc = zip64_parse_end_of_central_dir(info, data_start, dir_ofs,
                                        entry_count, pos - 20);

    /* Error or success? Bounce out of here. Keep going if not zip64. */
    if ((rc == 0) || (rc == 1))
        return rc;

    assert(rc == -1);  /* no error, just not a Zip64 archive. */

    /* Not Zip64? Seek back to where we were and keep processing. */
    BAIL_IF_MACRO(!io->seek(io, pos + 4), ERRPASS, 0);

    /* number of this disk */
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);
    BAIL_IF_MACRO(ui16 != 0, PHYSFS_ERR_CORRUPT, 0);

    /* number of the disk with the start of the central directory */
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);
    BAIL_IF_MACRO(ui16 != 0, PHYSFS_ERR_CORRUPT, 0);

    /* total number of entries in the central dir on this disk */
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);

    /* total number of entries in the central dir */
    BAIL_IF_MACRO(!readui16(io, &entryCount16), ERRPASS, 0);
    BAIL_IF_MACRO(ui16 != entryCount16, PHYSFS_ERR_CORRUPT, 0);

    *entry_count = entryCount16;

    /* size of the central directory */
    BAIL_IF_MACRO(!readui32(io, &ui32), ERRPASS, 0);

    /* offset of central directory */
    BAIL_IF_MACRO(!readui32(io, &offset32), ERRPASS, 0);
    *dir_ofs = (PHYSFS_uint64) offset32;
    BAIL_IF_MACRO(pos < (*dir_ofs + ui32), PHYSFS_ERR_CORRUPT, 0);

    /*
     * For self-extracting archives, etc, there's crapola in the file
     *  before the zipfile records; we calculate how much data there is
     *  prepended by determining how far the central directory offset is
     *  from where it is supposed to be (start of end-of-central-dir minus
     *  sizeof central dir)...the difference in bytes is how much arbitrary
     *  data is at the start of the physical file.
     */
    *data_start = (PHYSFS_uint64) (pos - (*dir_ofs + ui32));

    /* Now that we know the difference, fix up the central dir offset... */
    *dir_ofs += *data_start;

    /* zipfile comment length */
    BAIL_IF_MACRO(!readui16(io, &ui16), ERRPASS, 0);

    /*
     * Make sure that the comment length matches to the end of file...
     *  If it doesn't, we're either in the wrong part of the file, or the
     *  file is corrupted, but we give up either way.
     */
    BAIL_IF_MACRO((pos + 22 + ui16) != len, PHYSFS_ERR_CORRUPT, 0);

    return 1;  /* made it. */
} /* zip_parse_end_of_central_dir */


static int zip_alloc_hashtable(ZIPinfo *info, const PHYSFS_uint64 entry_count)
{
    size_t alloclen;

    info->hashBuckets = (size_t) (entry_count / 5);
    if (!info->hashBuckets)
        info->hashBuckets = 1;

    alloclen = info->hashBuckets * sizeof (ZIPentry *);
    info->hash = (ZIPentry **) allocator.Malloc(alloclen);
    BAIL_IF_MACRO(!info->hash, PHYSFS_ERR_OUT_OF_MEMORY, 0);
    memset(info->hash, '\0', alloclen);

    return 1;
} /* zip_alloc_hashtable */

static void ZIP_closeArchive(void *opaque);

static void *ZIP_openArchive(PHYSFS_Io *io, const char *name, int forWriting)
{
    ZIPinfo *info = NULL;
    PHYSFS_uint64 dstart;  /* data start */
    PHYSFS_uint64 cdir_ofs;  /* central dir offset */
    PHYSFS_uint64 entry_count;

    assert(io != NULL);  /* shouldn't ever happen. */

    BAIL_IF_MACRO(forWriting, PHYSFS_ERR_READ_ONLY, NULL);
    BAIL_IF_MACRO(!isZip(io), ERRPASS, NULL);

    info = (ZIPinfo *) allocator.Malloc(sizeof (ZIPinfo));
    BAIL_IF_MACRO(!info, PHYSFS_ERR_OUT_OF_MEMORY, NULL);
    memset(info, '\0', sizeof (ZIPinfo));
    info->root.resolved = ZIP_DIRECTORY;
    info->io = io;

    if (!zip_parse_end_of_central_dir(info, &dstart, &cdir_ofs, &entry_count))
        goto ZIP_openarchive_failed;
    else if (!zip_alloc_hashtable(info, entry_count))
        goto ZIP_openarchive_failed;
    else if (!zip_load_entries(info, dstart, cdir_ofs, entry_count))
        goto ZIP_openarchive_failed;

    assert(info->root.sibling == NULL);
    return info;

ZIP_openarchive_failed:
    info->io = NULL;  /* don't let ZIP_closeArchive destroy (io). */
    ZIP_closeArchive(info);
    return NULL;
} /* ZIP_openArchive */

static int ZIP_statEntry(const ZIPentry *entry, PHYSFS_Stat *stat);

static void ZIP_enumerateFiles(void *opaque, const char *dname,
                               PHYSFS_EnumFilesCallback cb,
                               const char *origdir, void *callbackdata)
{
    ZIPinfo *info = ((ZIPinfo *) opaque);
    const ZIPentry *entry = zip_find_entry(info, dname);
    if (entry && (entry->resolved == ZIP_DIRECTORY))
    {
        for (entry = entry->children; entry; entry = entry->sibling)
        {
            PHYSFS_Stat stat = {0};
            stat.filesize = -1;
            stat.filetype = (entry->resolved == ZIP_DIRECTORY) ? PHYSFS_FILETYPE_DIRECTORY : PHYSFS_FILETYPE_REGULAR;
            const char *ptr = strrchr(entry->name, '/');
            cb(callbackdata, origdir, ptr ? ptr + 1 : entry->name, &stat);
        } /* for */
    } /* if */
} /* ZIP_enumerateFiles */


static PHYSFS_Io *zip_get_io(PHYSFS_Io *io, ZIPinfo *inf, ZIPentry *entry)
{
    int success;
    PHYSFS_Io *retval = io->duplicate(io);
    BAIL_IF_MACRO(!retval, ERRPASS, NULL);

    /* !!! FIXME: if you open a dir here, it should bail ERR_NOT_A_FILE */

    /* (inf) can be NULL if we already resolved. */
    success = (inf == NULL) || zip_resolve(retval, inf, entry);
    if (success)
    {
        PHYSFS_sint64 offset;
        offset = ((entry->symlink) ? entry->symlink->offset : entry->offset);
        success = retval->seek(retval, offset);
    } /* if */

    if (!success)
    {
        retval->destroy(retval);
        retval = NULL;
    } /* if */

    return retval;
} /* zip_get_io */


static PHYSFS_Io *ZIP_openRead(void *opaque, const char *filename)
{
    PHYSFS_Io *retval = NULL;
    ZIPinfo *info = (ZIPinfo *) opaque;
    ZIPentry *entry = zip_find_entry(info, filename);
    ZIPfileinfo *finfo = NULL;
    PHYSFS_Io *io = NULL;
    PHYSFS_uint8 *password = NULL;

    /* if not found, see if maybe "$PASSWORD" is appended. */
    if ((!entry) && (info->has_crypto))
    {
        const char *ptr = strrchr(filename, '$');
        if (ptr != NULL)
        {
            const PHYSFS_uint64 len = (PHYSFS_uint64) (ptr - filename);
            char *str = (char *) __PHYSFS_smallAlloc(len + 1);
            BAIL_IF_MACRO(!str, PHYSFS_ERR_OUT_OF_MEMORY, NULL);
            memcpy(str, filename, len);
            str[len] = '\0';
            entry = zip_find_entry(info, str);
            __PHYSFS_smallFree(str);
            password = (PHYSFS_uint8 *) (ptr + 1);
        } /* if */
    } /* if */

    BAIL_IF_MACRO(!entry, ERRPASS, NULL);

    retval = (PHYSFS_Io *) allocator.Malloc(sizeof (PHYSFS_Io));
    GOTO_IF_MACRO(!retval, PHYSFS_ERR_OUT_OF_MEMORY, ZIP_openRead_failed);

    finfo = (ZIPfileinfo *) allocator.Malloc(sizeof (ZIPfileinfo));
    GOTO_IF_MACRO(!finfo, PHYSFS_ERR_OUT_OF_MEMORY, ZIP_openRead_failed);
    memset(finfo, '\0', sizeof (ZIPfileinfo));

    io = zip_get_io(info->io, info, entry);
    GOTO_IF_MACRO(!io, ERRPASS, ZIP_openRead_failed);
    finfo->io = io;
    finfo->entry = ((entry->symlink != NULL) ? entry->symlink : entry);
    initializeZStream(&finfo->stream);

    if (finfo->entry->compression_method != COMPMETH_NONE)
    {
        finfo->buffer = (PHYSFS_uint8 *) allocator.Malloc(ZIP_READBUFSIZE);
        if (!finfo->buffer)
            GOTO_MACRO(PHYSFS_ERR_OUT_OF_MEMORY, ZIP_openRead_failed);
        else if (zlib_err(inflateInit2(&finfo->stream, -MAX_WBITS)) != Z_OK)
            goto ZIP_openRead_failed;
    } /* if */

    if (!zip_entry_is_tradional_crypto(entry))
        GOTO_IF_MACRO(password != NULL, PHYSFS_ERR_BAD_PASSWORD, ZIP_openRead_failed);
    else
    {
        if (ZIP_IS_AES(entry)) {
            finfo->aes_ctx.encr_pos = AES_BLOCK_SIZE + 1;
        }
        else {
            PHYSFS_uint8 crypto_header[12];
            GOTO_IF_MACRO(password == NULL, PHYSFS_ERR_BAD_PASSWORD, ZIP_openRead_failed);
            if (io->read(io, crypto_header, 12) != 12)
                goto ZIP_openRead_failed;
            else if (!zip_prep_crypto_keys(finfo, crypto_header, password))
                goto ZIP_openRead_failed;
        }
    } /* if */

    memcpy(retval, &ZIP_Io, sizeof (PHYSFS_Io));
    retval->opaque = finfo;

    return retval;

ZIP_openRead_failed:
    if (finfo != NULL)
    {
        if (finfo->io != NULL)
            finfo->io->destroy(finfo->io);

        if (finfo->buffer != NULL)
        {
            allocator.Free(finfo->buffer);
            inflateEnd(&finfo->stream);
        } /* if */

        allocator.Free(finfo);
    } /* if */

    if (retval != NULL)
        allocator.Free(retval);

    return NULL;
} /* ZIP_openRead */


static PHYSFS_Io *ZIP_openWrite(void *opaque, const char *filename)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, NULL);
} /* ZIP_openWrite */


static PHYSFS_Io *ZIP_openAppend(void *opaque, const char *filename)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, NULL);
} /* ZIP_openAppend */


static void ZIP_closeArchive(void *opaque)
{
    ZIPinfo *info = (ZIPinfo *) (opaque);

    if (!info)
        return;

    if (info->io)
        info->io->destroy(info->io);

    assert(info->root.sibling == NULL);
    assert(info->hash || (info->root.children == NULL));

    if (info->hash)
    {
        size_t i;
        for (i = 0; i < info->hashBuckets; i++)
        {
            ZIPentry *entry;
            ZIPentry *next;
            for (entry = info->hash[i]; entry; entry = next)
            {
                next = entry->hashnext;
                allocator.Free(entry);
            } /* for */
        } /* for */
        allocator.Free(info->hash);
    } /* if */

    allocator.Free(info);
} /* ZIP_closeArchive */


static int ZIP_remove(void *opaque, const char *name)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, 0);
} /* ZIP_remove */


static int ZIP_mkdir(void *opaque, const char *name)
{
    BAIL_MACRO(PHYSFS_ERR_READ_ONLY, 0);
} /* ZIP_mkdir */

static int ZIP_statEntry(const ZIPentry *entry, PHYSFS_Stat *stat)
{
    /* !!! FIXME: does this need to resolve entries here? */

    if (entry == NULL)
        return 0;

    else if (entry->resolved == ZIP_DIRECTORY)
    {
        stat->filesize = 0;
        stat->filetype = PHYSFS_FILETYPE_DIRECTORY;
    } /* if */

    else if (zip_entry_is_symlink(entry))
    {
        stat->filesize = 0;
        stat->filetype = PHYSFS_FILETYPE_SYMLINK;
    } /* else if */

    else
    {
        stat->filesize = (PHYSFS_sint64) entry->uncompressed_size;
        stat->filetype = PHYSFS_FILETYPE_REGULAR;
    } /* else */

    stat->modtime = ((entry) ? entry->last_mod_time : 0);
    stat->createtime = stat->modtime;
    stat->accesstime = 0;
    stat->readonly = 1; /* .zip files are always read only */

    return 1;
} /* ZIP_stat */

static int ZIP_stat(void *opaque, const char *filename, PHYSFS_Stat *stat)
{
    ZIPinfo *info = (ZIPinfo *) opaque;
    const ZIPentry *entry = zip_find_entry(info, filename);

    return ZIP_statEntry(entry, stat);
} /* ZIP_stat */


const PHYSFS_Archiver __PHYSFS_Archiver_ZIP =
{
    CURRENT_PHYSFS_ARCHIVER_API_VERSION,
    {
        "ZIP",
        "PkZip/WinZip/Info-Zip compatible",
        "Ryan C. Gordon <icculus@icculus.org>",
        "https://icculus.org/physfs/",
        1,  /* supportsSymlinks */
    },
    ZIP_openArchive,
    ZIP_enumerateFiles,
    ZIP_openRead,
    ZIP_openWrite,
    ZIP_openAppend,
    ZIP_remove,
    ZIP_mkdir,
    ZIP_stat,
    ZIP_closeArchive
};

#endif  /* defined PHYSFS_SUPPORTS_ZIP */

/* end of archiver_zip.c ... */

