#include "spiffs.h"
#include "spiffs_nucleus.h"

#define SPIFFS_CHECK_RES(res) \
  do { \
    if ((res) < SPIFFS_OK) return (res); \
  } while (0);

#define _read(fs, a, s, d) (fs)->cfg.hal_read_f((a),(s),(d))
#define _write(fs, a, s, d) (fs)->cfg.hal_write_f((a),(s),(d))
#define SPIFFS_VALIDATE_OBJIX(ph, spix) \
    if (((ph).flags & SPIFFS_PH_FLAG_DELET) == 0) return SPIFFS_ERR_DELETED; \
    if (((ph).flags & SPIFFS_PH_FLAG_FINAL) != 0) return SPIFFS_ERR_NOT_FINALIZED; \
    if (((ph).flags & SPIFFS_PH_FLAG_INDEX) != 0) return SPIFFS_ERR_NOT_INDEX; \
    if ((ph).span_ix != (spix)) return SPIFFS_ERR_INDEX_SPAN_MISMATCH;

s32_t SPIFFS_init(spiffs *fs, spiffs_config *config, u8_t *work, u8_t *fd_space, u32_t fd_space_size) {
  memcpy(&fs->cfg, config, sizeof(spiffs_config));
  fs->block_count = fs->cfg.phys_size / fs->cfg.log_block_size;
  fs->work = &work[0];
  fs->lu_work = &work[fs->cfg.log_page_size];
  memset(fd_space, 0, fd_space_size);
  // align fd_space pointer
  if ((u32_t)fd_space & 3) {
    fd_space += (4-((u32_t)fd_space & 3));
    fd_space_size -= (4-((u32_t)fd_space & 3));
  }
  fs->fd_space = fd_space;
  fs->fd_count = (fd_space_size/sizeof(spiffs_fd));

  s32_t res = spiffs_phys_count_free_blocks(fs);
  SPIFFS_API_CHECK_RES(fs, res);

  SPIFFS_DBG("page index byte len:         %i\n", fs->cfg.log_page_size);
  SPIFFS_DBG("object lookup pages:         %i\n", SPIFFS_OBJ_LOOKUP_PAGES(fs));
  SPIFFS_DBG("page pages per block:        %i\n", SPIFFS_PAGES_PER_BLOCK(fs));
  SPIFFS_DBG("page header length:          %i\n", sizeof(spiffs_page_header));
  SPIFFS_DBG("object header index entries: %i\n", SPIFFS_OBJ_HDR_IX_LEN(fs));
  SPIFFS_DBG("object index entries:        %i\n", SPIFFS_OBJ_IX_LEN(fs));
  SPIFFS_DBG("available file descriptors:  %i\n", fs->fd_count);
  SPIFFS_DBG("free blocks:                 %i\n", fs->free_blocks);

  return 0;
}

s32_t SPIFFS_errno(spiffs *fs) {
  return fs->errno;
}

s32_t spiffs_phys_count_free_blocks(
    spiffs *fs) {
  s32_t res = SPIFFS_OK;
  u32_t blocks = fs->block_count;
  spiffs_block_ix cur_block = 0;
  fs->free_blocks = 0;
  while (res == SPIFFS_OK && blocks--) {
    spiffs_obj_id id;
    u32_t cur_block_addr = SPIFFS_BLOCK_TO_PADDR(fs, cur_block);
    res = _read(fs, cur_block_addr, sizeof(spiffs_obj_id), (u8_t *)&id);
    if (res != SPIFFS_OK) break;
    if (id == SPIFFS_OBJ_ID_FREE) {
      fs->free_blocks++;
    }
    cur_block++;
  }
  return res;
}

s32_t spiffs_phys_cpy(
    spiffs *fs,
    u32_t dst,
    u32_t src,
    u32_t len) {
  s32_t res;
  u8_t b[SPIFFS_COPY_BUFFER_STACK];
  while (len > 0) {
    u32_t chunk_size = MIN(SPIFFS_COPY_BUFFER_STACK, len);
    res = _read(fs, src, chunk_size, b);
    SPIFFS_CHECK_RES(res);
    res = _write(fs, dst, chunk_size, b);
    SPIFFS_CHECK_RES(res);
    len -= chunk_size;
    src += chunk_size;
    dst += chunk_size;
  }
  return SPIFFS_OK;
}

// Find object lookup entry containing given id with visitor.
// Iterate over object lookup pages in each block until a given object id entry is found.
// When found, the visitor function is called with block index, entry index and user_data.
// If visitor returns SPIFFS_CONTINUE, the search goes on. Otherwise, the search will be
// ended and visitor's return code is returned to caller.
// If no visitor is given (0) the search returns on first entry with matching object id, or
// when whole area is searched without finding object id.
s32_t spiffs_obj_lu_find_entry_visitor(
    spiffs *fs,
    spiffs_block_ix starting_block,
    int starting_index_entry,
    u8_t check_id,
    spiffs_obj_id obj_id,
    spiffs_visitor_f v,
    u32_t user_data,
    void *user_p,
    spiffs_block_ix *block_ix,
    int *index_entry) {
  s32_t res = SPIFFS_OK;
  s32_t entry_count = fs->block_count * SPIFFS_OBJ_LOOKUP_MAX_ENTRIES(fs);
  spiffs_block_ix cur_block = starting_block;
  u32_t cur_block_addr = SPIFFS_BLOCK_TO_PADDR(fs, starting_block);

  spiffs_obj_id *obj_lu_buf = (spiffs_obj_id *)fs->lu_work;
  int cur_entry = starting_index_entry;
  u32_t entries_per_page = (fs->cfg.log_page_size / sizeof(spiffs_obj_id));

  // wrap initial
  if (cur_entry >= SPIFFS_OBJ_LOOKUP_MAX_ENTRIES(fs) - 1) {
    cur_entry = 0;
    cur_block++;
    cur_block_addr = SPIFFS_BLOCK_TO_PADDR(fs, cur_block);
    if (cur_block >= fs->block_count) {
      // block wrap
      cur_block = 0;
      cur_block_addr = fs->cfg.phys_addr;
    }
  }

  // check each block
  while (res == SPIFFS_OK && entry_count > 0) {
    int obj_lookup_page = cur_entry / entries_per_page;
    // check each object lookup page
    while (res == SPIFFS_OK && obj_lookup_page < SPIFFS_OBJ_LOOKUP_PAGES(fs)) {
      int entry_offset = obj_lookup_page * entries_per_page;
      res = _read(fs, cur_block_addr + SPIFFS_PAGE_TO_PADDR(fs, obj_lookup_page), fs->cfg.log_page_size, fs->lu_work);
      // check each entry
      while (res == SPIFFS_OK &&
          cur_entry - entry_offset < entries_per_page && // for non-last obj lookup pages
          cur_entry < SPIFFS_OBJ_LOOKUP_MAX_ENTRIES(fs)) // for last obj lookup page
      {
        if (!check_id || obj_lu_buf[cur_entry-entry_offset] == obj_id) {
          if (block_ix) *block_ix = cur_block;
          if (index_entry) *index_entry = cur_entry;
          if (v) {
            res = v(fs, obj_lu_buf[cur_entry-entry_offset], cur_block, cur_entry, user_data, user_p);
            if (res == SPIFFS_COUNTINUE) {
              res = SPIFFS_OK;
              cur_entry++;
              entry_count--;
              continue;
            } else {
              return res;
            }
          }
          return SPIFFS_OK;
        }
        entry_count--;
        cur_entry++;
      } // per entry
      obj_lookup_page++;
    } // per object lookup page
    cur_entry = 0;
    cur_block++;
    cur_block_addr += fs->cfg.log_block_size;
    if (cur_block >= fs->block_count) {
      // block wrap
      cur_block = 0;
      cur_block_addr = fs->cfg.phys_addr;
    }
  } // per block

  SPIFFS_CHECK_RES(res);

  return SPIFFS_ERR_NOT_FOUND;
}


// Find free object lookup entry
// Iterate over object lookup pages in each block until a free object id entry is found
s32_t spiffs_obj_lu_find_free(
    spiffs *fs,
    spiffs_block_ix starting_block,
    int starting_index_entry,
    spiffs_block_ix *block_ix,
    int *index_entry) {
  s32_t res;
  res = spiffs_obj_lu_find_id(fs, starting_block, starting_index_entry,
      SPIFFS_OBJ_ID_FREE, block_ix, index_entry);
  if (res == SPIFFS_OK) {
    fs->free_cursor_block_ix = *block_ix;
    fs->free_cursor_obj_lu_entry = *index_entry;
    if (*index_entry == 0) {
      fs->free_blocks--;
    }
  }
  return res == SPIFFS_ERR_NOT_FOUND ? SPIFFS_ERR_FULL : res;
}

// Find object lookup entry containing given id
// Iterate over object lookup pages in each block until a given object id entry is found
s32_t spiffs_obj_lu_find_id(
    spiffs *fs,
    spiffs_block_ix starting_block,
    int starting_index_entry,
    spiffs_obj_id obj_id,
    spiffs_block_ix *block_ix,
    int *index_entry) {
  s32_t res = spiffs_obj_lu_find_entry_visitor(
      fs, starting_block, starting_index_entry, 1, obj_id, 0, 0, 0, block_ix, index_entry);
  return res;
}


static s32_t spiffs_obj_lu_find_id_and_index_v(
    spiffs *fs,
    spiffs_obj_id obj_id,
    spiffs_block_ix bix,
    int ix_entry,
    u32_t user_data,
    void *user_p) {
  s32_t res;
  spiffs_page_header ph;
  spiffs_page_ix pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, ix_entry);
  res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, pix), sizeof(spiffs_page_header), (u8_t *)&ph);
  SPIFFS_CHECK_RES(res);
  if (ph.span_ix == (spiffs_span_ix)user_data && ((ph.flags & (SPIFFS_PH_FLAG_FINAL | SPIFFS_PH_FLAG_DELET)) == SPIFFS_PH_FLAG_DELET)) {
    return SPIFFS_OK;
  } else {
    return SPIFFS_COUNTINUE;
  }
}

// Find object lookup entry containing given id and span index
// Iterate over object lookup pages in each block until a given object id entry is found
s32_t spiffs_obj_lu_find_id_and_index(
    spiffs *fs,
    spiffs_obj_id obj_id,
    spiffs_span_ix spix,
    spiffs_page_ix *pix) {
  s32_t res;
  spiffs_block_ix bix;
  int entry;

  res = spiffs_obj_lu_find_entry_visitor(fs,
      fs->cursor_block_ix,
      fs->cursor_obj_lu_entry,
      1,
      obj_id,
      spiffs_obj_lu_find_id_and_index_v,
      (u32_t)spix,
      0,
      &bix,
      &entry);

  SPIFFS_CHECK_RES(res);

  if (pix) {
    *pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, entry);
  }

  fs->cursor_block_ix = bix;
  fs->cursor_obj_lu_entry = entry;

  return res;
}

// Allocates a free defined page with given obj_id
// Occupies object lookup entry and page
// data may be NULL; where only page header is stored, len and page_offs is ignored
s32_t spiffs_page_allocate_data(
    spiffs *fs,
    spiffs_page_header *ph,
    u8_t *data,
    u32_t len,
    u32_t page_offs,
    u8_t finalize,
    spiffs_page_ix *pix) {
  s32_t res = SPIFFS_OK;
  spiffs_block_ix bix;
  int entry;

  // find free entry
  res = spiffs_obj_lu_find_free(fs, fs->free_cursor_block_ix, fs->free_cursor_obj_lu_entry, &bix, &entry);
  SPIFFS_CHECK_RES(res);

  // occupy page in object lookup
  res = _write(fs, SPIFFS_BLOCK_TO_PADDR(fs, bix) + entry * sizeof(spiffs_obj_id), sizeof(spiffs_obj_id), (u8_t*)&ph->obj_id);
  SPIFFS_CHECK_RES(res);

  // write page header
  res = _write(fs, SPIFFS_OBJ_LOOKUP_ENTRY_TO_PADDR(fs, bix, entry), sizeof(spiffs_page_header), (u8_t*)ph);
  SPIFFS_CHECK_RES(res);

  // write page data
  if (data) {
    res = _write(fs, SPIFFS_OBJ_LOOKUP_ENTRY_TO_PADDR(fs, bix, entry) + sizeof(spiffs_page_header) + page_offs, len, data);
    SPIFFS_CHECK_RES(res);
  }

  // finalize header if necessary
  if (finalize && (ph->flags & SPIFFS_PH_FLAG_FINAL)) {
    ph->flags &= ~SPIFFS_PH_FLAG_FINAL;
    res = _write(fs,
        SPIFFS_OBJ_LOOKUP_ENTRY_TO_PADDR(fs, bix, entry) + offsetof(spiffs_page_header, flags),
        sizeof(u8_t),
        (u8_t *)&ph->flags);
    SPIFFS_CHECK_RES(res);
  }

  // return written page
  if (pix) {
    *pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, entry);
  }

  return res;
}

// Moves a page from src to a free page and finalizes it. Updates page index. Page data is given in param page.
// If page data is null, provided header is used for metainfo and page data is physically copied.
s32_t spiffs_page_move(
    spiffs *fs,
    u8_t *page_data,
    spiffs_page_header *page_hdr,
    spiffs_page_ix src_pix,
    spiffs_page_ix *dst_pix) {
  s32_t res;
  u8_t was_final = 0;
  spiffs_page_header *p_hdr;
  spiffs_block_ix bix;
  int entry;
  spiffs_page_ix free_pix;

  // find free entry
  res = spiffs_obj_lu_find_free(fs, fs->free_cursor_block_ix, fs->free_cursor_obj_lu_entry, &bix, &entry);
  SPIFFS_CHECK_RES(res);
  free_pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, entry);

  if (dst_pix) *dst_pix = free_pix;

  p_hdr = page_data ? (spiffs_page_header *)page_data : page_hdr;
  if (page_data) {
    // got page data
    was_final = (p_hdr->flags & SPIFFS_PH_FLAG_FINAL) == 0;
    // write unfinalized page
    p_hdr->flags |= SPIFFS_PH_FLAG_FINAL;
    res = _write(fs, SPIFFS_PAGE_TO_PADDR(fs, free_pix), fs->cfg.log_page_size, page_data);
  } else {
    // copy page data
    res = spiffs_phys_cpy(fs, SPIFFS_PAGE_TO_PADDR(fs, free_pix), SPIFFS_PAGE_TO_PADDR(fs, src_pix), fs->cfg.log_page_size);
  }
  SPIFFS_CHECK_RES(res);
  // mark entry in destination object lookup
  res = _write(fs,
      SPIFFS_BLOCK_TO_PADDR(fs, SPIFFS_BLOCK_FOR_PAGE(fs, free_pix)) + SPIFFS_OBJ_LOOKUP_ENTRY_FOR_PAGE(fs, free_pix) * sizeof(spiffs_page_ix),
      sizeof(spiffs_obj_id),
      (u8_t *)&p_hdr->obj_id);
  SPIFFS_CHECK_RES(res);
  if (was_final) {
    // mark finalized in destination page
    p_hdr->flags &= ~SPIFFS_PH_FLAG_FINAL;
    res = _write(fs,
        SPIFFS_PAGE_TO_PADDR(fs, free_pix) + offsetof(spiffs_page_header, flags),
        sizeof(u8_t),
        (u8_t *)&p_hdr->flags);
    SPIFFS_CHECK_RES(res);
  }
  // mark source deleted
  res = spiffs_page_delete(fs, src_pix);
  return res;
}

// Deletes a page and removes it from object lookup.
s32_t spiffs_page_delete(
    spiffs *fs,
    spiffs_page_ix pix) {
  s32_t res;
  spiffs_page_header hdr;
  hdr.flags = 0xff & ~SPIFFS_PH_FLAG_DELET;
  // mark deleted entry in source object lookup
  spiffs_obj_id d_obj_id = SPIFFS_OBJ_ID_ERASED;
  res = _write(fs,
      SPIFFS_BLOCK_TO_PADDR(fs, SPIFFS_BLOCK_FOR_PAGE(fs, pix)) + SPIFFS_OBJ_LOOKUP_ENTRY_FOR_PAGE(fs, pix) * sizeof(spiffs_page_ix),
      sizeof(spiffs_obj_id),
      (u8_t *)&d_obj_id);
  SPIFFS_CHECK_RES(res);
  // mark deleted in source page
  res = _write(fs,
      SPIFFS_PAGE_TO_PADDR(fs, pix) + offsetof(spiffs_page_header, flags),
      sizeof(u8_t),
      (u8_t *)&hdr.flags);
  return res;
}

// Create an object index header page with empty index and undefined length
s32_t spiffs_object_create(
    spiffs *fs,
    spiffs_obj_id obj_id,
    u8_t name[SPIFFS_OBJ_NAME_LEN],
    spiffs_obj_type type,
    spiffs_page_ix *objix_hdr_pix) {
  s32_t res = SPIFFS_OK;
  spiffs_block_ix bix;
  spiffs_page_object_ix_header oix_hdr;
  int entry;

  res = spiffs_gc_check(fs);
  SPIFFS_CHECK_RES(res);

  obj_id |= SPIFFS_OBJ_ID_IX_FLAG;

  // find free entry
  res = spiffs_obj_lu_find_free(fs, fs->free_cursor_block_ix, fs->free_cursor_obj_lu_entry, &bix, &entry);
  SPIFFS_CHECK_RES(res);
  SPIFFS_DBG("found free page @ %04x bix:%i entry:%i\n", SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, entry), bix, entry);

  // occupy page in object lookup
  res = _write(fs, SPIFFS_BLOCK_TO_PADDR(fs, bix) + entry * sizeof(spiffs_obj_id), sizeof(spiffs_obj_id), (u8_t*)&obj_id);
  SPIFFS_CHECK_RES(res);

  // write empty object index page
  oix_hdr.p_hdr.obj_id = obj_id;
  oix_hdr.p_hdr.span_ix = 0;
  oix_hdr.p_hdr.flags = 0xff & ~(SPIFFS_PH_FLAG_FINAL | SPIFFS_PH_FLAG_INDEX);
  oix_hdr.type = type;
  oix_hdr.size = SPIFFS_UNDEFINED_LEN; // keep ones so we can update later without wasting this page
  strncpy((char *)&oix_hdr.name, (char *)name, SPIFFS_OBJ_NAME_LEN);


  // update page
  res = _write(fs, SPIFFS_OBJ_LOOKUP_ENTRY_TO_PADDR(fs, bix, entry), sizeof(spiffs_page_object_ix_header), (u8_t*)&oix_hdr);

  SPIFFS_CHECK_RES(res);
  spiffs_cb_object_event(fs, 0, SPIFFS_EV_IX_NEW, obj_id, 0, SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, entry), SPIFFS_UNDEFINED_LEN);

  if (objix_hdr_pix) {
    *objix_hdr_pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, entry);
  }

  return res;
}

// update object index header with any combination of name/size/index
// new_objix_hdr_data may be null, if so the object index header page is loaded
// name may be null, if so name is not changed
// size may be null, if so size is not changed
s32_t spiffs_object_update_index_hdr(
    spiffs *fs,
    spiffs_fd *fd,
    spiffs_page_ix objix_hdr_pix,
    u8_t *new_objix_hdr_data,
    u8_t name[SPIFFS_OBJ_NAME_LEN],
    u32_t size,
    spiffs_page_ix *new_pix) {
  s32_t res = SPIFFS_OK;
  spiffs_page_object_ix_header *objix_hdr;
  spiffs_page_ix new_objix_hdr_pix;

  if (new_objix_hdr_data) {
    // object index header page already given to us, no need to load it
    objix_hdr = (spiffs_page_object_ix_header *)new_objix_hdr_data;
  } else {
    // read object index header page
    res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, objix_hdr_pix), fs->cfg.log_page_size, fs->work);
    SPIFFS_CHECK_RES(res);
    objix_hdr = (spiffs_page_object_ix_header *)fs->work;
  }

  SPIFFS_VALIDATE_OBJIX(objix_hdr->p_hdr, 0);

  // change name
  if (name) {
    strncpy((char *)objix_hdr->name, (char *)name, SPIFFS_OBJ_NAME_LEN);
  }
  if (size) {
    objix_hdr->size = size;
  }

  // move and update page
  res = spiffs_page_move(fs, (u8_t*)objix_hdr, 0, objix_hdr_pix, &new_objix_hdr_pix);

  if (res == SPIFFS_OK) {
    if (new_pix) {
      *new_pix = new_objix_hdr_pix;
    }
    // callback on object index update
    spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_UPD, objix_hdr->p_hdr.obj_id, objix_hdr->p_hdr.span_ix, new_objix_hdr_pix, objix_hdr->size);
    if (fd) fd->objix_hdr_pix = new_objix_hdr_pix; // if this is not in the registered cluster
  }

  return res;
}

void spiffs_cb_object_event(
    spiffs *fs,
    spiffs_fd *fd,
    int ev,
    spiffs_obj_id obj_id,
    spiffs_span_ix spix,
    spiffs_page_ix new_pix,
    u32_t new_size) {
  // update caches in all file descriptors
  obj_id &= ~SPIFFS_OBJ_ID_IX_FLAG;
  int i;
  spiffs_fd *fds = (spiffs_fd *)fs->fd_space;
  for (i = 0; i < fs->fd_count; i++) {
    spiffs_fd *cur_fd = &fds[i];
    if (cur_fd->file_nbr == 0 || (cur_fd->obj_id & ~SPIFFS_OBJ_ID_IX_FLAG) != obj_id) continue;
    if (spix == 0) {
      if (ev == SPIFFS_EV_IX_NEW || ev == SPIFFS_EV_IX_UPD) {
        SPIFFS_DBG("       callback: setting fd %i:%04x objix_hdr_pix to %04x, size:%i\n", cur_fd->file_nbr, cur_fd->obj_id, new_pix, new_size);
        cur_fd->objix_hdr_pix = new_pix;
        if (new_size != 0) {
          cur_fd->size = new_size;
        }
      } else if (ev == SPIFFS_EV_IX_DEL) {
        cur_fd->obj_id = SPIFFS_OBJ_ID_ERASED;
      }
    }
    if (cur_fd->cursor_objix_spix == spix) {
      if (ev == SPIFFS_EV_IX_NEW || ev == SPIFFS_EV_IX_UPD) {
        SPIFFS_DBG("       callback: setting fd %i:%04x span:%04x objix_pix to %04x\n", cur_fd->file_nbr, cur_fd->obj_id, spix, new_pix);
        cur_fd->cursor_objix_pix = new_pix;
      } else {
        cur_fd->cursor_objix_pix = 0;
      }
    }
  }
}

// Open object by id
s32_t spiffs_object_open_by_id(
    spiffs *fs,
    spiffs_obj_id obj_id,
    spiffs_fd *fd,
    spiffs_attr attr,
    spiffs_mode mode) {
  s32_t res = SPIFFS_OK;
  spiffs_page_ix pix;

  res = spiffs_obj_lu_find_id_and_index(fs, obj_id, 0, &pix);
  SPIFFS_CHECK_RES(res);

  res = spiffs_object_open_by_page(fs, pix, fd, attr, mode);

  return res;
}

// Open object by page index
s32_t spiffs_object_open_by_page(
    spiffs *fs,
    spiffs_page_ix pix,
    spiffs_fd *fd,
    spiffs_attr attr,
    spiffs_mode mode) {
  s32_t res = SPIFFS_OK;
  spiffs_page_object_ix_header oix_hdr;

  res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, pix), sizeof(spiffs_page_object_ix_header), (u8_t *)&oix_hdr);
  SPIFFS_CHECK_RES(res);
  SPIFFS_VALIDATE_OBJIX(oix_hdr.p_hdr, 0);

  fd->fs = fs;
  fd->objix_hdr_pix = pix;
  fd->size = oix_hdr.size;
  fd->offset = 0;
  fd->cursor_objix_pix = pix;
  fd->cursor_objix_spix = 0;
  fd->obj_id = oix_hdr.p_hdr.obj_id;
  fd->mode = mode;

  SPIFFS_DBG("open: fd %i is obj id %04x\n", fd->file_nbr, fd->obj_id);

  return res;
}

// Append to object
// keep current object index (header) page in fs->work buffer
s32_t spiffs_object_append(spiffs_fd *fd, u32_t offset, u8_t *data, u32_t len) {
  spiffs *fs = fd->fs;
  s32_t res = SPIFFS_OK;
  u32_t written = 0;

  res = spiffs_gc_check(fs);
  SPIFFS_CHECK_RES(res);

  spiffs_page_object_ix_header *objix_hdr = (spiffs_page_object_ix_header *)fs->work;
  spiffs_page_object_ix *objix = (spiffs_page_object_ix *)fs->work;
  spiffs_page_header p_hdr;

  spiffs_span_ix cur_objix_spix = 0;
  spiffs_span_ix prev_objix_spix = (spiffs_span_ix)-1;
  spiffs_page_ix cur_objix_pix = fd->objix_hdr_pix;
  spiffs_page_ix new_objix_hdr_page;

  spiffs_span_ix data_spix = offset / SPIFFS_DATA_PAGE_SIZE(fs);
  spiffs_page_ix data_page;
  u32_t page_offs = offset % SPIFFS_DATA_PAGE_SIZE(fs);

  // write all data
  while (res == SPIFFS_OK && written < len) {
    // calculate object index page span index
    cur_objix_spix = SPIFFS_OBJ_IX_ENTRY_SPAN_IX(fs, data_spix);

    // handle storing and loading of object indices
    if (cur_objix_spix != prev_objix_spix) {
      // new object index page
      // within this clause we return directly if something fails, object index mess-up
      if (written > 0) {
        // store previous object index page, unless first pass
        SPIFFS_DBG("append: store objix %04x:%04x, written %i\n", cur_objix_pix, prev_objix_spix, written);
        if (prev_objix_spix == 0) {
          // this is an update to object index header page
          objix_hdr->size = offset+written;
          if (offset == 0) {
            // was an empty object, update same page (size was 0xffffffff)
            res = _write(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_objix_pix), fs->cfg.log_page_size, fs->work);
            SPIFFS_CHECK_RES(res);
          } else {
            // was a nonempty object, update to new page
            res = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, fs->work, 0, offset+written, &new_objix_hdr_page);
            SPIFFS_CHECK_RES(res);
            SPIFFS_DBG("append: store new objix_hdr, %04x:%04x, written %i\n", new_objix_hdr_page, 0, written);
          }
        } else {
          // this is an update to an object index page
          res = _write(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_objix_pix), fs->cfg.log_page_size, fs->work);
          SPIFFS_CHECK_RES(res);
          spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_UPD, objix->p_hdr.obj_id, objix->p_hdr.span_ix, cur_objix_pix, 0);
          // update length in object index header page
          res = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, 0, 0, offset+written, &new_objix_hdr_page);
          SPIFFS_CHECK_RES(res);
          SPIFFS_DBG("append: store new size in objix_hdr, %04x:%04x, written %i\n", new_objix_hdr_page, 0, written);
        }
        fd->size = offset+written;
        fd->offset = offset+written;
      }

      // create or load new object index page
      if (cur_objix_spix == 0) {
        // load object index header page, must always exist
        SPIFFS_DBG("append: load objixhdr page %04x:%04x\n", cur_objix_pix, cur_objix_spix);
        res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_objix_pix), fs->cfg.log_page_size, fs->work);
        SPIFFS_CHECK_RES(res);
        SPIFFS_VALIDATE_OBJIX(objix_hdr->p_hdr, cur_objix_spix);
      } else {
        if (written > 0) {
          // todo what if starting to append exactly where a new object index is needed?
          // on subsequent passes, create a new object index page
          p_hdr.obj_id = fd->obj_id | SPIFFS_OBJ_ID_IX_FLAG;
          p_hdr.span_ix = cur_objix_spix;
          p_hdr.flags = 0xff & ~(SPIFFS_PH_FLAG_FINAL | SPIFFS_PH_FLAG_INDEX);
          res = spiffs_page_allocate_data(fs, &p_hdr, 0, 0, 0, 1, &cur_objix_pix);
          SPIFFS_CHECK_RES(res);
          spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_NEW, fd->obj_id, cur_objix_spix, cur_objix_pix, 0);
          // quick "load" of new object index page
          memset(fs->work, 0xff, fs->cfg.log_page_size);
          memcpy(fs->work, &p_hdr, sizeof(spiffs_page_header));
          SPIFFS_DBG("append: create objix page, %04x:%04x, written %i\n", cur_objix_pix, cur_objix_spix, written);
        } else {
          // on first pass, we load existing object index page
          spiffs_page_ix pix;
          SPIFFS_DBG("append: find objix span_ix:%04x\n", cur_objix_spix);
          if (fd->cursor_objix_spix == cur_objix_spix) {
            pix = fd->cursor_objix_pix;
          } else {
            res = spiffs_obj_lu_find_id_and_index(fs, fd->obj_id | SPIFFS_OBJ_ID_IX_FLAG, cur_objix_spix, &pix);
            SPIFFS_CHECK_RES(res);
          }
          SPIFFS_DBG("append: found object index at page %04x\n", pix);
          res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, pix), fs->cfg.log_page_size, fs->work);
          SPIFFS_CHECK_RES(res);
          SPIFFS_VALIDATE_OBJIX(objix_hdr->p_hdr, cur_objix_spix);
          cur_objix_pix = pix;
        }
        fd->cursor_objix_pix = cur_objix_pix;
        fd->cursor_objix_spix = cur_objix_spix;
        fd->offset = offset+written;
        fd->size = offset+written;
      }
      prev_objix_spix = cur_objix_spix;
    }

    // write data
    u32_t to_write = MIN(len-written, SPIFFS_DATA_PAGE_SIZE(fs) - page_offs);
    if (page_offs == 0) {
      // at beginning of a page, allocate and write a new page of data
      p_hdr.obj_id = fd->obj_id & ~SPIFFS_OBJ_ID_IX_FLAG;
      p_hdr.span_ix = data_spix;
      p_hdr.flags = 0xff & ~(SPIFFS_PH_FLAG_FINAL);  // finalize immediately
      res = spiffs_page_allocate_data(fs, &p_hdr, &data[written], to_write, page_offs, 1, &data_page);
      SPIFFS_DBG("append: store new data page, %04x:%04x offset:%i, len %i, written %i\n", data_page, data_spix, page_offs, to_write, written);
    } else {
      // append to existing page, fill out free data in existing page
      if (cur_objix_spix == 0) {
        // get data page from object index header page
        data_page = ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix];
      } else {
        // get data page from object index page
        data_page = ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)];
      }
      res = _write(fs, SPIFFS_PAGE_TO_PADDR(fs, data_page) + sizeof(spiffs_page_header) + page_offs, to_write, &data[written]);
      SPIFFS_DBG("append: store to existing data page, %04x:%04x offset:%i, len %i, written %i\n", data_page, data_spix, page_offs, to_write, written);
    }

    if (res != SPIFFS_OK) break;

    // update memory representation of object index page with new data page
    if (cur_objix_spix == 0) {
      // update object index header page
      ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix] = data_page;
      SPIFFS_DBG("append: wrote page %04x to objix_hdr entry %02x in mem\n", data_page, data_spix);
      objix_hdr->size = offset+written;
    } else {
      // update object index page
      ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)] = data_page;
      SPIFFS_DBG("append: wrote page %04x to objix entry %02x in mem\n", data_page, SPIFFS_OBJ_IX_ENTRY(fs, data_spix));
    }

    // update internals
    page_offs = 0;
    data_spix++;
    written += to_write;
  } // while all data

  fd->size = offset+written;
  fd->offset = offset+written;
  fd->cursor_objix_pix = cur_objix_pix;
  fd->cursor_objix_spix = cur_objix_spix;

  // finalize updated object indices
  s32_t res2 = SPIFFS_OK;
  if (cur_objix_spix != 0) {
    // wrote beyond object index header page
    // write last modified object index page, unless object header index page
    SPIFFS_DBG("append: store objix page, %04x:%04x, written %i\n", cur_objix_pix, cur_objix_spix, written);
    res2 = _write(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_objix_pix), fs->cfg.log_page_size, fs->work);
    SPIFFS_CHECK_RES(res2);
    spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_UPD, objix->p_hdr.obj_id, objix->p_hdr.span_ix, cur_objix_pix, 0);

    // update size in object header index page
    SPIFFS_DBG("append: store new size in objix_hdr, %04x:%04x, written %i\n", new_objix_hdr_page, 0, written);
    res2 = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, 0, 0, offset+written, &new_objix_hdr_page);
    SPIFFS_CHECK_RES(res2);
  } else {
    // wrote within object index header page
    if (offset == 0) {
      // wrote to empty object - simply update size and write whole page
      objix_hdr->size = offset+written;
      SPIFFS_DBG("append: store fresh objix_hdr page, %04x:%04x, written %i\n", cur_objix_pix, cur_objix_spix, written);
      res2 = _write(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_objix_pix), fs->cfg.log_page_size, fs->work);
      SPIFFS_CHECK_RES(res2);
    } else {
      // modifying object index header page, update size and make new copy
      SPIFFS_DBG("append: store modified objix_hdr page, %04x:%04x, written %i\n", new_objix_hdr_page, 0, written);
      res2 = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, fs->work, 0, offset+written, &new_objix_hdr_page);
      SPIFFS_CHECK_RES(res2);
    }
  }

  return res;
}

// Modify object
// keep current object index (header) page in fs->work buffer
s32_t spiffs_object_modify(spiffs_fd *fd, u32_t offset, u8_t *data, u32_t len) {
  spiffs *fs = fd->fs;
  s32_t res = SPIFFS_OK;
  u32_t written = 0;

  res = spiffs_gc_check(fs);
  SPIFFS_CHECK_RES(res);

  spiffs_page_object_ix_header *objix_hdr = (spiffs_page_object_ix_header *)fs->work;
  spiffs_page_object_ix *objix = (spiffs_page_object_ix *)fs->work;
  spiffs_page_header p_hdr;

  spiffs_span_ix cur_objix_spix = 0;
  spiffs_span_ix prev_objix_spix = (spiffs_span_ix)-1;
  spiffs_page_ix cur_objix_pix = fd->objix_hdr_pix;
  spiffs_page_ix new_objix_hdr_pix;

  spiffs_span_ix data_spix = offset / SPIFFS_DATA_PAGE_SIZE(fs);
  spiffs_page_ix data_pix;
  u32_t page_offs = offset % SPIFFS_DATA_PAGE_SIZE(fs);


  // write all data
  while (res == SPIFFS_OK && written < len) {
    // calculate object index page span index
    cur_objix_spix = SPIFFS_OBJ_IX_ENTRY_SPAN_IX(fs, data_spix);

    // handle storing and loading of object indices
    if (cur_objix_spix != prev_objix_spix) {
      // new object index page
      // within this clause we return directly if something fails, object index mess-up
      if (written > 0) {
        // store previous object index (header) page, unless first pass
        if (prev_objix_spix == 0) {
          // store previous object index header page
          res = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, fs->work, 0, 0, &new_objix_hdr_pix);
          SPIFFS_DBG("modify: store modified objix_hdr page, %04x:%04x, written %i\n", new_objix_hdr_pix, 0, written);
          SPIFFS_CHECK_RES(res);
        } else {
          // store new version of previous object index page
          spiffs_page_ix new_objix_pix;
          res = spiffs_page_move(fs, (u8_t*)objix, 0, cur_objix_pix, &new_objix_pix);
          SPIFFS_DBG("modify: store previous modified objix page, %04x:%04x, written %i\n", new_objix_pix, objix->p_hdr.span_ix, written);
          SPIFFS_CHECK_RES(res);
          spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_UPD, objix->p_hdr.obj_id, objix->p_hdr.span_ix, new_objix_pix, 0);
        }
      }

      // load next object index page
      if (cur_objix_spix == 0) {
        // load object index header page, must exist
        SPIFFS_DBG("modify: load objixhdr page %04x:%04x\n", cur_objix_pix, cur_objix_spix);
        res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_objix_pix), fs->cfg.log_page_size, fs->work);
        SPIFFS_CHECK_RES(res);
        SPIFFS_VALIDATE_OBJIX(objix_hdr->p_hdr, cur_objix_spix);
      } else {
        // load existing object index page on first pass
        spiffs_page_ix pix;
        SPIFFS_DBG("modify: find objix span_ix:%04x\n", cur_objix_spix);
        if (fd->cursor_objix_spix == cur_objix_spix) {
          pix = fd->cursor_objix_pix;
        } else {
          res = spiffs_obj_lu_find_id_and_index(fs, fd->obj_id | SPIFFS_OBJ_ID_IX_FLAG, cur_objix_spix, &pix);
          SPIFFS_CHECK_RES(res);
        }
        SPIFFS_DBG("modify: found object index at page %04x\n", pix);
        res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, pix), fs->cfg.log_page_size, fs->work);
        SPIFFS_CHECK_RES(res);
        SPIFFS_VALIDATE_OBJIX(objix_hdr->p_hdr, cur_objix_spix);
        cur_objix_pix = pix;
      }
      fd->cursor_objix_pix = cur_objix_pix;
      fd->cursor_objix_spix = cur_objix_spix;
      fd->offset = offset+written;
      prev_objix_spix = cur_objix_spix;
    }

    // write partial data
    u32_t to_write = MIN(len-written, SPIFFS_DATA_PAGE_SIZE(fs) - page_offs);
    spiffs_page_ix orig_data_pix;
    if (cur_objix_spix == 0) {
      // get data page from object index header page
      orig_data_pix = ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix];
    } else {
      // get data page from object index page
      orig_data_pix = ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)];
    }

    p_hdr.obj_id = fd->obj_id & ~SPIFFS_OBJ_ID_IX_FLAG;
    p_hdr.span_ix = data_spix;
    p_hdr.flags = 0xff;
    p_hdr.flags = 0xff;
    if (page_offs == 0 && to_write == SPIFFS_DATA_PAGE_SIZE(fs)) {
      // a full page, allocate and write a new page of data
      res = spiffs_page_allocate_data(fs, &p_hdr, &data[written], to_write, page_offs, 1, &data_pix);
      SPIFFS_DBG("modify: store new data page, %04x:%04x offset:%i, len %i, written %i\n", data_pix, data_spix, page_offs, to_write, written);
    } else {
      // write to existing page, allocate new and copy unmodified data
      res = spiffs_page_allocate_data(fs, &p_hdr, 0, 0, 0, 0, &data_pix);
      if (res != SPIFFS_OK) break;
      // copy unmodified data
      if (page_offs > 0) {
        // before modification
        res = spiffs_phys_cpy(fs,
            SPIFFS_PAGE_TO_PADDR(fs, data_pix) + sizeof(spiffs_page_header),
            SPIFFS_PAGE_TO_PADDR(fs, orig_data_pix) + sizeof(spiffs_page_header),
            page_offs);
        if (res != SPIFFS_OK) break;
      }
      if (page_offs + to_write < SPIFFS_DATA_PAGE_SIZE(fs)) {
        // after modification
        res = spiffs_phys_cpy(fs,
            SPIFFS_PAGE_TO_PADDR(fs, data_pix) + sizeof(spiffs_page_header) + page_offs + to_write,
            SPIFFS_PAGE_TO_PADDR(fs, orig_data_pix) + sizeof(spiffs_page_header) + page_offs + to_write,
            SPIFFS_DATA_PAGE_SIZE(fs) - (page_offs + to_write));
        if (res != SPIFFS_OK) break;
      }

      res = _write(fs, SPIFFS_PAGE_TO_PADDR(fs, data_pix) + sizeof(spiffs_page_header) + page_offs, to_write, &data[written]);
      if (res != SPIFFS_OK) break;
      p_hdr.flags &= ~SPIFFS_PH_FLAG_FINAL;
      res = _write(fs,
          SPIFFS_PAGE_TO_PADDR(fs, data_pix) + offsetof(spiffs_page_header, flags),
          sizeof(u8_t),
          (u8_t *)&p_hdr.flags);
      if (res != SPIFFS_OK) break;

      SPIFFS_DBG("modify: store to existing data page, src:%04x, dst:%04x:%04x offset:%i, len %i, written %i\n", orig_data_pix, data_pix, data_spix, page_offs, to_write, written);
    }

    // delete original data page
    res = spiffs_page_delete(fs, orig_data_pix);
    if (res != SPIFFS_OK) break;
    // update memory representation of object index page with new data page
    if (cur_objix_spix == 0) {
      // update object index header page
      ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix] = data_pix;
      SPIFFS_DBG("modify: wrote page %04x to objix_hdr entry %02x in mem\n", data_pix, data_spix);
    } else {
      // update object index page
      ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)] = data_pix;
      SPIFFS_DBG("modify: wrote page %04x to objix entry %02x in mem\n", data_pix, SPIFFS_OBJ_IX_ENTRY(fs, data_spix));
    }

    // update internals
    page_offs = 0;
    data_spix++;
    written += to_write;
  } // while all data

  fd->offset = offset+written;
  fd->cursor_objix_pix = cur_objix_pix;
  fd->cursor_objix_spix = cur_objix_spix;

  // finalize updated object indices
  s32_t res2 = SPIFFS_OK;
  if (cur_objix_spix != 0) {
    // wrote beyond object index header page
    // write last modified object index page
    // move and update page
    spiffs_page_ix new_objix_pix;

    res2 = spiffs_page_move(fs, (u8_t*)objix, 0, cur_objix_pix, &new_objix_pix);
    SPIFFS_DBG("modify: store modified objix page, %04x:%04x, written %i\n", new_objix_pix, cur_objix_spix, written);
    fd->cursor_objix_pix = new_objix_pix;
    fd->cursor_objix_spix = cur_objix_spix;
    SPIFFS_CHECK_RES(res2);
    spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_UPD, objix->p_hdr.obj_id, objix->p_hdr.span_ix, new_objix_pix, 0);

  } else {
    // wrote within object index header page
    res2 = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, fs->work, 0, 0, &new_objix_hdr_pix);
    SPIFFS_DBG("modify: store modified objix_hdr page, %04x:%04x, written %i\n", new_objix_hdr_pix, 0, written);
    SPIFFS_CHECK_RES(res2);
  }

  return res;
}

static s32_t spiffs_object_find_object_index_header_by_name_v(
    spiffs *fs,
    spiffs_obj_id obj_id,
    spiffs_block_ix bix,
    int ix_entry,
    u32_t user_data,
    void *user_p) {
  s32_t res;
  spiffs_page_object_ix_header objix_hdr;
  spiffs_page_ix pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, ix_entry);
  if (obj_id == SPIFFS_OBJ_ID_FREE || obj_id == SPIFFS_OBJ_ID_ERASED ||
      (obj_id & SPIFFS_OBJ_ID_IX_FLAG) == 0) {
    return SPIFFS_COUNTINUE;
  }
  res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, pix), sizeof(spiffs_page_object_ix_header), (u8_t *)&objix_hdr);
  SPIFFS_CHECK_RES(res);
  if ((objix_hdr.p_hdr.obj_id & SPIFFS_OBJ_ID_IX_FLAG) &&
      objix_hdr.p_hdr.span_ix == 0 &&
      (objix_hdr.p_hdr.flags& (SPIFFS_PH_FLAG_DELET | SPIFFS_PH_FLAG_FINAL)) == SPIFFS_PH_FLAG_DELET) {
    if (strncmp((char *)user_p, (char *)objix_hdr.name, SPIFFS_OBJ_NAME_LEN) == 0) {
      return SPIFFS_OK;
    }
  }

  return SPIFFS_COUNTINUE;
}

// Finds object index header page by name
s32_t spiffs_object_find_object_index_header_by_name(
    spiffs *fs,
    u8_t name[SPIFFS_OBJ_NAME_LEN],
    spiffs_page_ix *pix) {
  s32_t res;
  spiffs_block_ix bix;
  int entry;

  res = spiffs_obj_lu_find_entry_visitor(fs,
      fs->cursor_block_ix,
      fs->cursor_obj_lu_entry,
      0,
      0,
      spiffs_object_find_object_index_header_by_name_v,
      0,
      name,
      &bix,
      &entry);

  SPIFFS_CHECK_RES(res);

  if (pix) {
    *pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, entry);
  }

  fs->cursor_block_ix = bix;
  fs->cursor_obj_lu_entry = entry;

  return res;
}

// Truncates object to new size. If new size is null, object may be removed totally
s32_t spiffs_object_truncate(
    spiffs_fd *fd,
    u32_t new_size,
    u8_t remove) {
  s32_t res = SPIFFS_OK;
  spiffs *fs = fd->fs;

  res = spiffs_gc_check(fs);
  SPIFFS_CHECK_RES(res);

  spiffs_page_ix objix_pix = fd->objix_hdr_pix;
  spiffs_span_ix data_spix = fd->size / SPIFFS_DATA_PAGE_SIZE(fs);
  u32_t cur_size = fd->size == SPIFFS_UNDEFINED_LEN ? 0 : fd->size ;
  spiffs_span_ix cur_objix_spix = 0;
  spiffs_span_ix prev_objix_spix = (spiffs_span_ix)-1;
  spiffs_page_object_ix_header *objix_hdr = (spiffs_page_object_ix_header *)fs->work;
  spiffs_page_object_ix *objix = (spiffs_page_object_ix *)fs->work;
  spiffs_page_ix data_pix;
  spiffs_page_ix new_objix_hdr_pix;

  // delete from end of object until desired len is reached
  while (cur_size > new_size) {
    cur_objix_spix = SPIFFS_OBJ_IX_ENTRY_SPAN_IX(fs, data_spix);

    // put object index for current data span index in work buffer
    if (prev_objix_spix != cur_objix_spix) {
      if (prev_objix_spix != (spiffs_span_ix)-1) {
        // remove previous object index page
        SPIFFS_DBG("truncate: delete objix page %04x:%04x\n", objix_pix, prev_objix_spix);
        res = spiffs_page_delete(fs, objix_pix);
        SPIFFS_CHECK_RES(res);
        spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_DEL, objix->p_hdr.obj_id, objix->p_hdr.span_ix, objix_pix, 0);
        if (prev_objix_spix > 0) {
          // update object index header page
          SPIFFS_DBG("truncate: update objix hdr page %04x:%04x to size %i\n", fd->objix_hdr_pix, prev_objix_spix, cur_size);
          res = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, 0, 0, cur_size, &new_objix_hdr_pix);
          SPIFFS_CHECK_RES(res);
          fd->size = cur_size;
        }
      }
      // load current object index (header) page
      if (cur_objix_spix == 0) {
        objix_pix = fd->objix_hdr_pix;
      } else {
        res = spiffs_obj_lu_find_id_and_index(fs, fd->obj_id | SPIFFS_OBJ_ID_IX_FLAG, cur_objix_spix, &objix_pix);
        SPIFFS_CHECK_RES(res);
      }
      SPIFFS_DBG("truncate: load objix page %04x:%04x for data spix:%04x\n", objix_pix, cur_objix_spix, data_spix);
      res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, objix_pix), fs->cfg.log_page_size, fs->work);
      SPIFFS_CHECK_RES(res);
      SPIFFS_VALIDATE_OBJIX(objix_hdr->p_hdr, cur_objix_spix);
      fd->cursor_objix_pix = objix_pix;
      fd->cursor_objix_spix = cur_objix_spix;
      fd->offset = cur_size;

      prev_objix_spix = cur_objix_spix;
    }

    if (cur_objix_spix == 0) {
      // get data page from object index header page
      data_pix = ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix];
      ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix] = SPIFFS_OBJ_ID_FREE;
    } else {
      // get data page from object index page
      data_pix = ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)];
      ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)] = SPIFFS_OBJ_ID_FREE;
    }

    if (cur_size - SPIFFS_DATA_PAGE_SIZE(fs) >= new_size) {
      // delete full data page
      res = spiffs_page_delete(fs, data_pix);
      if (res != SPIFFS_OK) break;
      // update current size
      if (cur_size % SPIFFS_DATA_PAGE_SIZE(fs) == 0) {
        cur_size -= SPIFFS_DATA_PAGE_SIZE(fs);
      } else {
        cur_size -= cur_size % SPIFFS_DATA_PAGE_SIZE(fs);
      }
      fd->size = cur_size;
      fd->offset = cur_size;
      SPIFFS_DBG("truncate: delete data page %04x for data spix:%04x, cur_size:%i\n", data_pix, data_spix, cur_size);
    } else {
      // delete last page, partially
      spiffs_page_header p_hdr;
      spiffs_page_ix new_data_pix;
      u32_t bytes_to_remove = SPIFFS_DATA_PAGE_SIZE(fs) - (new_size % SPIFFS_DATA_PAGE_SIZE(fs));
      SPIFFS_DBG("truncate: delete %i bytes from data page %04x for data spix:%04x, cur_size:%i\n", bytes_to_remove, data_pix, data_spix, cur_size);

      p_hdr.obj_id = fd->obj_id & ~SPIFFS_OBJ_ID_IX_FLAG;
      p_hdr.span_ix = data_spix;
      p_hdr.flags = 0xff;
      // allocate new page and copy unmodified data
      res = spiffs_page_allocate_data(fs, &p_hdr, 0, 0, 0, 0, &new_data_pix);
      if (res != SPIFFS_OK) break;
      res = spiffs_phys_cpy(fs,
          SPIFFS_PAGE_TO_PADDR(fs, new_data_pix) + sizeof(spiffs_page_header),
          SPIFFS_PAGE_TO_PADDR(fs, data_pix) + sizeof(spiffs_page_header),
          SPIFFS_DATA_PAGE_SIZE(fs) - bytes_to_remove);
      if (res != SPIFFS_OK) break;
      // delete original data page
      res = spiffs_page_delete(fs, data_pix);
      if (res != SPIFFS_OK) break;
      p_hdr.flags &= ~SPIFFS_PH_FLAG_FINAL;
      res = _write(fs,
          SPIFFS_PAGE_TO_PADDR(fs, new_data_pix) + offsetof(spiffs_page_header, flags),
          sizeof(u8_t),
          (u8_t *)&p_hdr.flags);
      if (res != SPIFFS_OK) break;

      // update memory representation of object index page with new data page
      if (cur_objix_spix == 0) {
        // update object index header page
        ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix] = new_data_pix;
        SPIFFS_DBG("truncate: wrote page %04x to objix_hdr entry %02x in mem\n", new_data_pix, SPIFFS_OBJ_IX_ENTRY(fs, data_spix));
      } else {
        // update object index page
        ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)] = new_data_pix;
        SPIFFS_DBG("truncate: wrote page %04x to objix entry %02x in mem\n", new_data_pix, SPIFFS_OBJ_IX_ENTRY(fs, data_spix));
      }
      cur_size = new_size;
      fd->size = new_size;
      fd->offset = cur_size;
      break;
    }
    data_spix--;
  } // while all data

  // update object indices
  if (cur_objix_spix == 0) {
    // update object index header page
    if (cur_size == 0) {
      if (remove) {
        // remove object altogether
        SPIFFS_DBG("truncate: remove object index header page %04x\n", objix_pix);
        res = spiffs_page_delete(fs, objix_pix);
        SPIFFS_CHECK_RES(res);
        spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_DEL, objix_hdr->p_hdr.obj_id, objix_hdr->p_hdr.span_ix, objix_pix, 0);
      } else {
        // make uninitialized object
        SPIFFS_DBG("truncate: reset objix_hdr page %04x\n", objix_pix);
        memset(fs->work + sizeof(spiffs_page_object_ix_header), 0xff,
            fs->cfg.log_page_size - sizeof(spiffs_page_object_ix_header));
        res = spiffs_object_update_index_hdr(fs, fd, objix_pix, fs->work, 0, SPIFFS_UNDEFINED_LEN, &new_objix_hdr_pix);
        SPIFFS_CHECK_RES(res);
      }
    } else {
      // update object index header page
      SPIFFS_DBG("truncate: update object index header page with indices and size\n");
      res = spiffs_object_update_index_hdr(fs, fd, objix_pix, fs->work, 0, cur_size, &new_objix_hdr_pix);
      SPIFFS_CHECK_RES(res);
    }
  } else {
    // update both current object index page and object index header page
    spiffs_page_ix new_objix_pix;

    // move and update object index page
    res = spiffs_page_move(fs, (u8_t*)objix_hdr, 0, objix_pix, &new_objix_pix);
    SPIFFS_CHECK_RES(res);
    spiffs_cb_object_event(fs, fd, SPIFFS_EV_IX_UPD, objix->p_hdr.obj_id, objix->p_hdr.span_ix, new_objix_pix, 0);
    SPIFFS_DBG("truncate: store modified objix page, %04x:%04x\n", new_objix_pix, cur_objix_spix);
    fd->cursor_objix_pix = new_objix_pix;
    fd->cursor_objix_spix = cur_objix_spix;
    fd->offset = cur_size;
    // update object index header page with new size
    res = spiffs_object_update_index_hdr(fs, fd, fd->objix_hdr_pix, 0, 0, cur_size, &new_objix_hdr_pix);
    SPIFFS_CHECK_RES(res);
  }
  fd->size = cur_size;

  return res;
}

s32_t spiffs_object_read(
    spiffs_fd *fd,
    u32_t offset,
    u32_t len,
    u8_t *dst) {
  s32_t res = SPIFFS_OK;
  spiffs *fs = fd->fs;
  spiffs_page_ix objix_pix;
  spiffs_page_ix data_pix;
  spiffs_span_ix data_spix = offset / SPIFFS_DATA_PAGE_SIZE(fs);
  u32_t cur_offset = offset;
  spiffs_span_ix cur_objix_spix;
  spiffs_span_ix prev_objix_spix = (spiffs_span_ix)-1;
  spiffs_page_object_ix_header *objix_hdr = (spiffs_page_object_ix_header *)fs->work;
  spiffs_page_object_ix *objix = (spiffs_page_object_ix *)fs->work;

  while (cur_offset < offset + len) {
    cur_objix_spix = SPIFFS_OBJ_IX_ENTRY_SPAN_IX(fs, data_spix);
    if (prev_objix_spix != cur_objix_spix) {
      // load current object index (header) page
      if (cur_objix_spix == 0) {
        objix_pix = fd->objix_hdr_pix;
      } else {
        res = spiffs_obj_lu_find_id_and_index(fs, fd->obj_id | SPIFFS_OBJ_ID_IX_FLAG, cur_objix_spix, &objix_pix);
        SPIFFS_CHECK_RES(res);
      }
      SPIFFS_DBG("read: load objix page %04x:%04x for data spix:%04x\n", objix_pix, cur_objix_spix, data_spix);
      res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, objix_pix), fs->cfg.log_page_size, fs->work);
      SPIFFS_CHECK_RES(res);
      SPIFFS_VALIDATE_OBJIX(objix->p_hdr, cur_objix_spix);

      fd->offset = cur_offset;
      fd->cursor_objix_pix = objix_pix;
      fd->cursor_objix_spix = cur_objix_spix;

      prev_objix_spix = cur_objix_spix;
    }

    if (cur_objix_spix == 0) {
      // get data page from object index header page
      data_pix = ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[data_spix];
    } else {
      // get data page from object index page
      data_pix = ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, data_spix)];
    }

    // all remaining data
    u32_t len_to_read = offset + len - cur_offset;
    // remaining data in page
    len_to_read = MIN(len_to_read, SPIFFS_DATA_PAGE_SIZE(fs) - (cur_offset % SPIFFS_DATA_PAGE_SIZE(fs)));
    // remaining data in file
    len_to_read = MIN(len_to_read, fd->size);
    //SPIFFS_DBG("read: offset:%i rd:%i data spix:%04x is data_pix:%04x\n", cur_offset, len_to_read, data_spix, data_pix);
    if (len_to_read <= 0) {
      res = SPIFFS_ERR_END_OF_OBJECT;
      break;
    }
    res = _read(
        fs,
        SPIFFS_PAGE_TO_PADDR(fs, data_pix) + sizeof(spiffs_page_header) + (cur_offset % SPIFFS_DATA_PAGE_SIZE(fs)),
        len_to_read,
        dst);
    SPIFFS_CHECK_RES(res);
    dst += len_to_read;
    cur_offset += len_to_read;
    fd->offset = cur_offset;
    data_spix++;
  }

  return res;
}

s32_t spiffs_gc_check(
    spiffs *fs) {
  s32_t res;
  if (fs->free_blocks > 2) {
    return SPIFFS_OK;
  }
  spiffs_block_ix *cands;
  int count;
  spiffs_block_ix cand;
  res = spiffs_gc_find_candidate(fs, &cands, &count);
  SPIFFS_CHECK_RES(res);
  if (count == 0) {
    return res;
  }
  cand = cands[0];
  res = spiffs_gc_clean(fs, cand);
  SPIFFS_CHECK_RES(res);

  u32_t addr = SPIFFS_BLOCK_TO_PADDR(fs, cand);
  s32_t size = fs->cfg.log_block_size;

  // here we ignore res, just try erasing the block
  SPIFFS_DBG("gc_check: erase block %i\n", cand);
  while (size > 0) {
    SPIFFS_DBG("gc_check: erase %08x:%08x\n", addr, fs->cfg.phys_erase_block);
    res = fs->cfg.hal_erase_f(addr, fs->cfg.phys_erase_block);
    addr += fs->cfg.phys_erase_block;
    size -= fs->cfg.phys_erase_block;
  }
  fs->free_blocks++;

  return res;
}

s32_t spiffs_gc_find_candidate(
    spiffs *fs,
    spiffs_block_ix **block_candidates,
    int *candidate_count) {
  s32_t res = SPIFFS_OK;
  u32_t blocks = fs->block_count;
  spiffs_block_ix cur_block = 0;
  u32_t cur_block_addr = fs->cfg.phys_addr;
  spiffs_obj_id *obj_lu_buf = (spiffs_obj_id *)fs->lu_work;
  int cur_entry = 0;

  // using fs->work area as sorted candidate memory, (spiffs_block_ix)cand_bix/(int)score
  int max_candidates = MIN(fs->block_count, (fs->cfg.log_page_size-8)/(sizeof(spiffs_block_ix) + sizeof(int)));
  *candidate_count = 0;
  memset(fs->work, 0xff, fs->cfg.log_page_size);

  // divide up work area into block indices and scores
  // todo alignment?
  spiffs_block_ix *cand_blocks = (spiffs_block_ix *)fs->work;
  int *cand_scores = (int *)(fs->work + max_candidates * sizeof(spiffs_block_ix));

  *block_candidates = cand_blocks;

  u32_t entries_per_page = (fs->cfg.log_page_size / sizeof(spiffs_obj_id));

  // check each block
  while (res == SPIFFS_OK && blocks--) {
    u16_t deleted_pages_in_block = 0;
    u16_t used_pages_in_block = 0;

    int obj_lookup_page = 0;
    // check each object lookup page
    while (res == SPIFFS_OK && obj_lookup_page < SPIFFS_OBJ_LOOKUP_PAGES(fs)) {
      int entry_offset = obj_lookup_page * entries_per_page;
      res = _read(fs, cur_block_addr + SPIFFS_PAGE_TO_PADDR(fs, obj_lookup_page), fs->cfg.log_page_size, fs->lu_work);
      // check each entry
      while (res == SPIFFS_OK &&
          cur_entry - entry_offset < entries_per_page && cur_entry < SPIFFS_PAGES_PER_BLOCK(fs)-SPIFFS_OBJ_LOOKUP_PAGES(fs)) {
        spiffs_obj_id obj_id = obj_lu_buf[cur_entry-entry_offset];
        if (obj_id == SPIFFS_OBJ_ID_FREE) {
          // when a free entry is encountered, scan logic ensures that all following entries are free also
          break;
        } else  if (obj_id == SPIFFS_OBJ_ID_ERASED) {
          deleted_pages_in_block++;
        } else {
          used_pages_in_block++;
        }
        cur_entry++;
      } // per entry
      obj_lookup_page++;
    } // per object lookup page

    // calculate score and insert into candidate table
    // stoneage sort, but probably not so many blocks
    if (res == SPIFFS_OK && deleted_pages_in_block > 0) {
      int score = deleted_pages_in_block * SPIFFS_GC_HEUR_W_DELET + used_pages_in_block * SPIFFS_GC_HEUR_W_USED;
      int cand_ix = 0;
      SPIFFS_DBG("gc_check: bix:%i del:%i use:%i score:%i\n", cur_block, deleted_pages_in_block, used_pages_in_block, score);
      while (cand_ix < max_candidates) {
        if (cand_blocks[cand_ix] == (spiffs_block_ix)-1) {
          cand_blocks[cand_ix] = cur_block;
          cand_scores[cand_ix] = score;
          break;
        } else if (cand_scores[cand_ix] < score) {
          int reorder_cand_ix = max_candidates - 2;
          while (reorder_cand_ix >= cand_ix) {
            cand_blocks[reorder_cand_ix + 1] = cand_blocks[reorder_cand_ix];
            cand_scores[reorder_cand_ix + 1] = cand_scores[reorder_cand_ix];
            reorder_cand_ix--;
          }
          cand_blocks[cand_ix] = cur_block;
          cand_scores[cand_ix] = score;
          break;
        }
        cand_ix++;
      }
      (*candidate_count)++;
    }

    cur_entry = 0;
    cur_block++;
    cur_block_addr += fs->cfg.log_block_size;
  } // per block

  return res;
}

typedef enum {
  MOVE_OBJ_IX,
  FIND_OBJ_DATA,
  MOVE_OBJ_DATA,
  FINISHED
} spiffs_gc_clean_state;

typedef struct {
  spiffs_gc_clean_state state;
  spiffs_obj_id cur_obj_id;
  spiffs_span_ix cur_objix_spix;
  spiffs_page_ix cur_objix_pix;
  int stored_scan_entry_index;
  u8_t obj_id_found;
} spiffs_gc;

// Empties given block by moving all data into free pages of another block
// Strategy:
//   scan object lookup for object index pages, move to new page in other block
//   loop:
//   scan object lookup for object data pages
//   for first found id, check spix and load corresponding object index page to memory
//   push object scan lookup entry index
//     rescan object lookup, find data pages with same id and referenced by same object index
//     move data page, update object index in memory
//     when reached end of lookup, store updated object index
//   pop object scan lookup entry index
//   repeat loop until end of object lookup
//
s32_t spiffs_gc_clean(spiffs *fs, spiffs_block_ix bix) {
  s32_t res = SPIFFS_OK;
  u32_t entries_per_page = (fs->cfg.log_page_size / sizeof(spiffs_obj_id));
  int cur_entry = 0;
  spiffs_obj_id *obj_lu_buf = (spiffs_obj_id *)fs->lu_work;
  spiffs_gc gc;
  spiffs_page_ix cur_pix = 0;
  spiffs_page_object_ix_header *objix_hdr = (spiffs_page_object_ix_header *)fs->work;
  spiffs_page_object_ix *objix = (spiffs_page_object_ix *)fs->work;

  memset(&gc, 0, sizeof(spiffs_gc));
  gc.state = MOVE_OBJ_IX;

  if (fs->free_cursor_block_ix == bix) {
    // move free cursor to next block
    fs->free_cursor_block_ix = (bix+1)%fs->block_count;
    fs->free_cursor_obj_lu_entry = 0;
    SPIFFS_DBG("gc_clean: move free cursor to block %i\n", fs->free_cursor_block_ix);
  }

  while (res == SPIFFS_OK && gc.state != FINISHED) {
    SPIFFS_DBG("gc_clean: state = %i entry:%i\n", gc.state, cur_entry);
    gc.obj_id_found = 0;

    // scan through lookup pages
    int obj_lookup_page = cur_entry / entries_per_page;
    u8_t scan = 1;
    // check each object lookup page
    while (scan && res == SPIFFS_OK && obj_lookup_page < SPIFFS_OBJ_LOOKUP_PAGES(fs)) {
      int entry_offset = obj_lookup_page * entries_per_page;
      res = _read(fs, SPIFFS_BLOCK_TO_PADDR(fs, bix) + SPIFFS_PAGE_TO_PADDR(fs, obj_lookup_page),
          fs->cfg.log_page_size, fs->lu_work);
      // check each entry
      while (scan && res == SPIFFS_OK &&
          cur_entry - entry_offset < entries_per_page && cur_entry < SPIFFS_PAGES_PER_BLOCK(fs)-SPIFFS_OBJ_LOOKUP_PAGES(fs)) {
        spiffs_obj_id obj_id = obj_lu_buf[cur_entry-entry_offset];
        cur_pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, cur_entry);

        // act upon object id depending on gc state
        switch (gc.state) {
        case MOVE_OBJ_IX:
          if (obj_id != SPIFFS_OBJ_ID_ERASED && obj_id != SPIFFS_OBJ_ID_FREE && (obj_id & SPIFFS_OBJ_ID_IX_FLAG)) {
            // found an index object id
            spiffs_page_header p_hdr;
            spiffs_page_ix new_pix;
            // load header
            res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_pix), sizeof(spiffs_page_header), (u8_t*)&p_hdr);
            SPIFFS_CHECK_RES(res);
            if (p_hdr.flags & SPIFFS_PH_FLAG_DELET) {
              // move page
              res = spiffs_page_move(fs, 0, &p_hdr, cur_pix, &new_pix);
              SPIFFS_DBG("gc_clean: MOVE_OBJIX move objix %04x:%04x page %04x to %04x\n", obj_id, p_hdr.span_ix, cur_pix, new_pix);
              SPIFFS_CHECK_RES(res);
              spiffs_cb_object_event(fs, 0, SPIFFS_EV_IX_UPD, obj_id, p_hdr.span_ix, new_pix, 0);
              // move wipes obj_lu, reload it
              res = _read(fs, SPIFFS_BLOCK_TO_PADDR(fs, bix) + SPIFFS_PAGE_TO_PADDR(fs, obj_lookup_page),
                  fs->cfg.log_page_size, fs->lu_work);
              SPIFFS_CHECK_RES(res);
            } else {
              // page is deleted but not deleted in lookup, scrap it
              SPIFFS_DBG("gc_clean: MOVE_OBJIX wipe objix %04x:%04x page %04x\n", obj_id, p_hdr.span_ix, cur_pix);
              res = spiffs_page_delete(fs, cur_pix);
              if (res == SPIFFS_OK) {
                spiffs_cb_object_event(fs, 0, SPIFFS_EV_IX_DEL, obj_id, p_hdr.span_ix, cur_pix, 0);
              }
            }
            SPIFFS_CHECK_RES(res);
          }
          break;
        case FIND_OBJ_DATA:
          if (obj_id != SPIFFS_OBJ_ID_ERASED && obj_id != SPIFFS_OBJ_ID_FREE) {
            SPIFFS_DBG("gc_clean: FIND_DATA state:%i - found obj id %04x\n", gc.state, obj_id);
            gc.obj_id_found = 1;
            gc.cur_obj_id = obj_id;
            scan = 0;
          }
          break;
        case MOVE_OBJ_DATA:
          if (obj_id == gc.cur_obj_id) {
            spiffs_page_header p_hdr;
            res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_pix), sizeof(spiffs_page_header), (u8_t*)&p_hdr);
            SPIFFS_CHECK_RES(res);
            SPIFFS_DBG("gc_clean: MOVE_DATA found data page %04x:%04x @ %04x\n", gc.cur_obj_id, p_hdr.span_ix, cur_pix);
            if (SPIFFS_OBJ_IX_ENTRY_SPAN_IX(fs, p_hdr.span_ix) != gc.cur_objix_spix) {
              SPIFFS_DBG("gc_clean: MOVE_DATA no objix spix match, take in another run\n", gc.cur_obj_id, p_hdr.span_ix, cur_pix);
            } else {
              spiffs_page_ix new_data_pix;
              if (p_hdr.flags & SPIFFS_PH_FLAG_DELET) {
                // move page
                res = spiffs_page_move(fs, 0, &p_hdr, cur_pix, &new_data_pix);
                SPIFFS_DBG("gc_clean: MOVE_DATA move objix %04x:%04x page %04x to %04x\n", gc.cur_obj_id, p_hdr.span_ix, cur_pix, new_data_pix);
                SPIFFS_CHECK_RES(res);
                // move wipes obj_lu, reload it
                res = _read(fs, SPIFFS_BLOCK_TO_PADDR(fs, bix) + SPIFFS_PAGE_TO_PADDR(fs, obj_lookup_page),
                    fs->cfg.log_page_size, fs->lu_work);
                SPIFFS_CHECK_RES(res);
              } else {
                // page is deleted but not deleted in lookup, scrap it
                SPIFFS_DBG("gc_clean: MOVE_DATA wipe objix %04x:%04x page %04x\n", obj_id, p_hdr.span_ix, cur_pix);
                res = spiffs_page_delete(fs, cur_pix);
                SPIFFS_CHECK_RES(res);
                new_data_pix = SPIFFS_OBJ_ID_FREE;
              }
              // update memory representation of object index page with new data page
              if (gc.cur_objix_spix == 0) {
                // update object index header page
                ((spiffs_page_ix*)((void*)objix_hdr + sizeof(spiffs_page_object_ix_header)))[p_hdr.span_ix] = new_data_pix;
                SPIFFS_DBG("gc_clean: MOVE_DATA wrote page %04x to objix_hdr entry %02x in mem\n", new_data_pix, SPIFFS_OBJ_IX_ENTRY(fs, p_hdr.span_ix));
              } else {
                // update object index page
                ((spiffs_page_ix*)((void*)objix + sizeof(spiffs_page_object_ix)))[SPIFFS_OBJ_IX_ENTRY(fs, p_hdr.span_ix)] = new_data_pix;
                SPIFFS_DBG("gc_clean: MOVE_DATA wrote page %04x to objix entry %02x in mem\n", new_data_pix, SPIFFS_OBJ_IX_ENTRY(fs, p_hdr.span_ix));
              }
            }
          }
          break;
        default:
          scan = 0;
          break;
        }
        cur_entry++;
      } // per entry
      obj_lookup_page++;
    } // per object lookup page

    if (res != SPIFFS_OK) break;

    // state finalization and switch
    switch (gc.state) {
    case MOVE_OBJ_IX:
      gc.state = FIND_OBJ_DATA;
      cur_entry = 0; // restart entry scan index
      break;
    case FIND_OBJ_DATA:
      if (gc.obj_id_found) {
        // find out corresponding obj ix page and load it to memory
        spiffs_page_header p_hdr;
        spiffs_page_ix objix_pix;
        gc.stored_scan_entry_index = cur_entry;
        cur_entry = 0;
        gc.state = MOVE_OBJ_DATA;
        res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, cur_pix), sizeof(spiffs_page_header), (u8_t*)&p_hdr);
        SPIFFS_CHECK_RES(res);
        gc.cur_objix_spix = SPIFFS_OBJ_IX_ENTRY_SPAN_IX(fs, p_hdr.span_ix);
        SPIFFS_DBG("gc_clean: FIND_DATA find objix span_ix:%04x\n", gc.cur_objix_spix);
        res = spiffs_obj_lu_find_id_and_index(fs, gc.cur_obj_id | SPIFFS_OBJ_ID_IX_FLAG, gc.cur_objix_spix, &objix_pix);
        SPIFFS_CHECK_RES(res);
        SPIFFS_DBG("gc_clean: FIND_DATA found object index at page %04x\n", objix_pix);
        res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, objix_pix), fs->cfg.log_page_size, fs->work);
        SPIFFS_CHECK_RES(res);
        SPIFFS_VALIDATE_OBJIX(objix->p_hdr, gc.cur_objix_spix);
        gc.cur_objix_pix = objix_pix;
      } else {
        gc.state = FINISHED;
      }
      break;
    case MOVE_OBJ_DATA: {
      // store modified objix (hdr) page
      spiffs_page_ix new_objix_pix;
      gc.state = FIND_OBJ_DATA;
      cur_entry = gc.stored_scan_entry_index;
      if (gc.cur_objix_spix == 0) {
        // store object index header page
        res = spiffs_object_update_index_hdr(fs, 0, gc.cur_objix_pix, fs->work, 0, 0, &new_objix_pix);
        SPIFFS_DBG("gc_clean: MOVE_DATA store modified objix_hdr page, %04x:%04x\n", new_objix_pix, 0);
        SPIFFS_CHECK_RES(res);
      } else {
        // store object index page
        spiffs_page_ix new_objix_pix;
        res = spiffs_page_move(fs, fs->work, 0, gc.cur_objix_pix, &new_objix_pix);
        SPIFFS_DBG("gc_clean: MOVE_DATA store modified objix page, %04x:%04x\n", new_objix_pix, objix->p_hdr.span_ix);
        SPIFFS_CHECK_RES(res);
        spiffs_cb_object_event(fs, 0, SPIFFS_EV_IX_UPD, gc.cur_obj_id, objix->p_hdr.span_ix, new_objix_pix, 0);
      }
    }
    break;
    default:
      cur_entry = 0;
      break;
    }
    SPIFFS_DBG("gc_clean: state-> %i\n", gc.state);
  } // while state != FINISHED


  return res;
}

typedef struct {
  spiffs_obj_id min_obj_id;
  spiffs_obj_id max_obj_id;
  u32_t compaction;
} spiffs_free_obj_id_state;

static s32_t spiffs_obj_lu_find_free_obj_id_bitmap_v(spiffs *fs, spiffs_obj_id id, spiffs_block_ix bix, int ix_entry,
    u32_t user_data, void *user_p) {
  if (id != SPIFFS_OBJ_ID_FREE && id != SPIFFS_OBJ_ID_ERASED) {
    spiffs_obj_id min_obj_id = user_data;
    id &= ~SPIFFS_OBJ_ID_IX_FLAG;
    int bit_ix = (id-min_obj_id) & 7;
    int byte_ix = (id-min_obj_id) >> 3;
    if (byte_ix >= 0 && byte_ix < fs->cfg.log_page_size) {
      fs->work[byte_ix] |= (1<<bit_ix);
    }
  }
  return SPIFFS_COUNTINUE;
}

static s32_t spiffs_obj_lu_find_free_obj_id_compact_v(spiffs *fs, spiffs_obj_id id, spiffs_block_ix bix, int ix_entry,
    u32_t user_data, void *user_p) {
  if (id != SPIFFS_OBJ_ID_FREE && id != SPIFFS_OBJ_ID_ERASED && (id & SPIFFS_OBJ_ID_IX_FLAG)) {
    s32_t res;
    spiffs_free_obj_id_state *state = (spiffs_free_obj_id_state *)user_p;
    spiffs_page_header ph;

    res = _read(fs, SPIFFS_OBJ_LOOKUP_ENTRY_TO_PADDR(fs, bix, ix_entry), sizeof(spiffs_page_header), (u8_t*)&ph);
    if (res == SPIFFS_OK && ph.span_ix == 0 &&
        ((ph.flags & (SPIFFS_PH_FLAG_INDEX | SPIFFS_PH_FLAG_FINAL | SPIFFS_PH_FLAG_DELET)) == (SPIFFS_PH_FLAG_DELET))) {
      // ok object look up entry
      id &= ~SPIFFS_OBJ_ID_IX_FLAG;
      if (id >= state->min_obj_id && id <= state->max_obj_id) {
        u8_t *map = (u8_t *)fs->work;
        int ix = (id - state->min_obj_id) / state->compaction;
        //SPIFFS_DBG("free_obj_id: add ix %i for id %04x min:%04x max%04x comp:%i\n", ix, id, state->min_obj_id, state->max_obj_id, state->compaction);
        map[ix]++;
      }
    }
  }
  return SPIFFS_COUNTINUE;
}

s32_t spiffs_obj_lu_find_free_obj_id(spiffs *fs, spiffs_obj_id *obj_id) {
  s32_t res = SPIFFS_OK;
  u32_t max_objects = ((fs->cfg.phys_size) / (u32_t)fs->cfg.log_page_size) / 2;
  spiffs_free_obj_id_state state;
  spiffs_obj_id free_obj_id = SPIFFS_OBJ_ID_FREE;
  state.min_obj_id = 1;
  state.max_obj_id = max_objects + 1;
  if (state.max_obj_id & SPIFFS_OBJ_ID_IX_FLAG) {
    state.max_obj_id = ((spiffs_obj_id)-1) & ~SPIFFS_OBJ_ID_IX_FLAG;
  }
  state.compaction = 0;
  while (res == SPIFFS_OK && free_obj_id == SPIFFS_OBJ_ID_FREE) {
    if (state.max_obj_id - state.min_obj_id <= fs->cfg.log_page_size*8) {
      // possible to represent in bitmap
      int i, j;
      SPIFFS_DBG("free_obj_id: BITM min:%04x max:%04x\n", state.min_obj_id, state.max_obj_id);

      memset(fs->work, 0, fs->cfg.log_page_size);
      res = spiffs_obj_lu_find_entry_visitor(fs, 0, 0, 0, 0, spiffs_obj_lu_find_free_obj_id_bitmap_v, state.min_obj_id, 0, 0, 0);
      if (res == SPIFFS_ERR_NOT_FOUND) res = SPIFFS_OK;
      SPIFFS_CHECK_RES(res);
      // traverse bitmask until found free obj_id
      for (i = 0; i < fs->cfg.log_page_size; i++) {
        u8_t mask = fs->work[i];
        if (mask == 0xff) {
          continue;
        }
        for (j = 0; j < 8; j++) {
          if ((mask & (1<<j)) == 0) {
            *obj_id = (i<<3)+j+state.min_obj_id;
            return SPIFFS_OK;
          }
        }
      }
      return SPIFFS_ERR_FULL;
    } else {
      // not possible to represent all ids in range in a bitmap, compact and count
      if (state.compaction != 0) {
        // select element in compacted table, decrease range and recompact
        int i, min_i = 0;
        u8_t *map = (u8_t *)fs->work;
        u8_t min_count = 0xff;

        for (i = 0; i < fs->cfg.log_page_size/sizeof(u8_t); i++) {
          if (map[i] < min_count) {
            min_count = map[i];
            min_i = i;
            if (min_count == 0) {
              break;
            }
          }
        }

        if (min_count == state.compaction) {
          // there are no free objids!
          SPIFFS_DBG("free_obj_id: compacted table is full\n");
          return SPIFFS_ERR_FULL;
        }

        SPIFFS_DBG("free_obj_id: COMP select index:%i min_count:%i min:%04x max:%04x compact:%i\n", min_i, min_count, state.min_obj_id, state.max_obj_id, state.compaction);

        if (min_count == 0) {
          // no id in this range, skip compacting and use directly
          *obj_id = min_i * state.compaction + state.min_obj_id;
          return SPIFFS_OK;
        } else {
          SPIFFS_DBG("free_obj_id: COMP SEL chunk:%04x min:%04x -> %04x\n", state.compaction, state.min_obj_id, state.min_obj_id + min_i *  state.compaction);
          state.min_obj_id += min_i *  state.compaction;
          state.max_obj_id = state.min_obj_id + state.compaction;
          // decrease compaction
        }
        if ((state.max_obj_id - state.min_obj_id <= fs->cfg.log_page_size*8)) {
          // no need for compacting, use bitmap
          continue;
        }
      }
      // in a work memory of log_page_size bytes, we may fit in log_page_size ids
      // todo what if compaction is > 255 - then we cannot fit it in a byte
      state.compaction = (state.max_obj_id-state.min_obj_id) / ((fs->cfg.log_page_size / sizeof(u8_t)));
      SPIFFS_DBG("free_obj_id: COMP min:%04x max:%04x compact:%i\n", state.min_obj_id, state.max_obj_id, state.compaction);

      memset(fs->work, 0, fs->cfg.log_page_size);
      res = spiffs_obj_lu_find_entry_visitor(fs, 0, 0, 0, 0, spiffs_obj_lu_find_free_obj_id_compact_v, 0, &state, 0, 0);
      if (res == SPIFFS_ERR_NOT_FOUND) res = SPIFFS_OK;
      SPIFFS_CHECK_RES(res);
    }
  }

  return res;
}

s32_t spiffs_fd_find_new(spiffs *fs, spiffs_fd **fd) {
  int i;
  spiffs_fd *fds = (spiffs_fd *)fs->fd_space;
  for (i = 0; i < fs->fd_count; i++) {
    spiffs_fd *cur_fd = &fds[i];
    if (cur_fd->file_nbr == 0) {
      cur_fd->file_nbr = i+1;
      *fd = cur_fd;
      return SPIFFS_OK;
    }
  }
  return SPIFFS_ERR_OUT_OF_FILE_DESCS;
}

s32_t spiffs_fd_return(spiffs *fs, spiffs_file f) {
  if (f <= 0 || f > fs->fd_count) {
    return SPIFFS_ERR_BAD_DESCRIPTOR;
  }
  spiffs_fd *fds = (spiffs_fd *)fs->fd_space;
  spiffs_fd *fd = &fds[f-1];
  if (fd->file_nbr == 0) {
    return SPIFFS_ERR_FILE_CLOSED;
  }
  fd->file_nbr = 0;
  return SPIFFS_OK;
}

s32_t spiffs_fd_get(spiffs *fs, spiffs_file f, spiffs_fd **fd) {
  if (f <= 0 || f > fs->fd_count) {
    return SPIFFS_ERR_BAD_DESCRIPTOR;
  }
  spiffs_fd *fds = (spiffs_fd *)fs->fd_space;
  *fd = &fds[f-1];
  if ((*fd)->file_nbr == 0) {
    return SPIFFS_ERR_FILE_CLOSED;
  }
  return SPIFFS_OK;
}

static s32_t spiffs_test_list_object_v(
    spiffs *fs,
    spiffs_obj_id obj_id,
    spiffs_block_ix bix,
    int ix_entry,
    u32_t user_data,
    void *user_p) {
  s32_t res;
  spiffs_page_object_ix_header objix_hdr;
  if (obj_id == SPIFFS_OBJ_ID_FREE || obj_id == SPIFFS_OBJ_ID_ERASED ||
      (obj_id & SPIFFS_OBJ_ID_IX_FLAG) == 0) {
    return SPIFFS_COUNTINUE;
  }

  spiffs_page_ix pix = SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, ix_entry);
  res = _read(fs, SPIFFS_PAGE_TO_PADDR(fs, pix), sizeof(spiffs_page_object_ix_header), (u8_t *)&objix_hdr);
  SPIFFS_CHECK_RES(res);
  if ((objix_hdr.p_hdr.obj_id & SPIFFS_OBJ_ID_IX_FLAG) &&
      objix_hdr.p_hdr.span_ix == 0 &&
      (objix_hdr.p_hdr.flags& (SPIFFS_PH_FLAG_DELET | SPIFFS_PH_FLAG_FINAL)) == SPIFFS_PH_FLAG_DELET) {
    print("spiffs_list block:%i page:%04x obj_id:%04x '%s' size:%i bytes\n",
        bix,
        SPIFFS_OBJ_LOOKUP_ENTRY_TO_PIX(fs, bix, ix_entry),
        objix_hdr.p_hdr.obj_id,
        objix_hdr.name,
        objix_hdr.size);
  }

  return SPIFFS_COUNTINUE;
}

void spiffs_test_list_objects(spiffs *fs) {
  spiffs_block_ix bix;
  int entry;

  spiffs_obj_lu_find_entry_visitor(fs,
      0,
      0,
      0,
      0,
      spiffs_test_list_object_v,
      0,
      0,
      &bix,
      &entry);
}
