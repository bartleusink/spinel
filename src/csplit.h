#ifndef SPINEL_CSPLIT_H
#define SPINEL_CSPLIT_H
#include <stddef.h>

/* Split the preprocessed unit at pre_path into out_dir/sp_split.h and
   nparts part files (their paths written to part_paths). Returns nparts, or
   -1 when the unit holds something the splitter does not read, in which
   case the caller compiles the single unit. See csplit.c. */
int c_split(const char *pre_path, const char *out_dir, int nparts,
            char (*part_paths)[4096], char *hdr_path, size_t hdr_sz);

#endif
