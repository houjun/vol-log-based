#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
// Std hdrs
#include <array>
#include <cassert>
#include <cstring>
// Sys hdrs
#include <dirent.h>
#include <mpi.h>
#include <sys/stat.h>
#include <unistd.h>
// Logvol hdrs
#include "H5VL_log.h"
#include "H5VL_log_dataset.hpp"
#include "H5VL_log_file.hpp"
#include "H5VL_log_filei.hpp"
#include "H5VL_logi.hpp"
#include "H5VL_logi_util.hpp"
#include "H5VL_logi_wrapper.hpp"
#include "H5VL_logi_zip.hpp"

//#define DEFAULT_SIZE 1073741824 // 1 GiB
#define DEFAULT_SIZE 209715200	// 200 MiB
//#define DEFAULT_SIZE 10485760 // 10 MiB

herr_t H5VL_log_filei_balloc (H5VL_log_file_t *fp, size_t size, void **buf) {
	herr_t err = 0;
	size_t *bp;

	// printf("Balloc %llu\n", size);

	if (fp->bsize != LOG_VOL_BSIZE_UNLIMITED) {
		if (fp->bused + size > (size_t) (fp->bsize)) {
			err	 = -1;
			*buf = NULL;
			ERR_OUT ("Out of buffer")
		}
	}

	// The first sizeof (size_t) bytes are used to store the size of the buffer allocated so we can
	// keep track of the total amount of buffer allocated
	bp = (size_t *)malloc (size + sizeof (size_t));
	if (bp == NULL) {
		err	 = -1;
		*buf = NULL;
		ERR_OUT ("OOM")
	}
	*bp	 = size;
	*buf = bp + 1;

	fp->bused += size;

err_out:;
	return err;
}

herr_t H5VL_log_filei_post_open (H5VL_log_file_t *fp) {
	herr_t err = 0;
	int mpierr;
	H5VL_loc_params_t loc;
	H5VL_object_specific_args_t args;
	int attbuf[5];

	H5VL_LOGI_PROFILING_TIMER_START;

	// Att
	err = H5VL_logi_get_att (fp, "_int_att", H5T_NATIVE_INT32, attbuf, fp->dxplid);
	CHECK_ERR
	fp->ndset  = attbuf[0];
	fp->nldset = attbuf[1];
	fp->nmdset = attbuf[2];
	fp->config = attbuf[3];
	fp->ngroup = attbuf[3];
	fp->idx.resize (fp->ndset);
	fp->mreqs.resize (fp->ndset);
	fp->dsets.resize (fp->ndset);
	fp->group_rank = fp->rank;
	fp->group_comm = fp->comm;
	fp->group_id   = 0;

	H5VL_LOGI_PROFILING_TIMER_START;
	if (fp->config & H5VL_FILEI_CONFIG_SUBFILING) {
		err = H5VL_log_filei_calc_node_rank (fp);
		CHECK_ERR
	} else {
		fp->group_rank = fp->rank;
		fp->group_np   = fp->np;
		fp->group_comm = fp->comm;
		fp->group_id   = 0;
		fp->ngroup	   = 1;
	}
	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VL_LOG_FILE_OPEN_GROUP_RANK);

	H5VL_LOGI_PROFILING_TIMER_START;
	if (fp->config & H5VL_FILEI_CONFIG_SUBFILING) {
		// Aligned write not supported in subfiles
		fp->config &= ~H5VL_FILEI_CONFIG_DATA_ALIGN;

		err = H5VL_log_filei_open_subfile (fp, fp->flag, fp->uvlid, fp->dxplid);
		CHECK_ERR
	} else {
		fp->sfp		= fp->uo;
		fp->subname = std::string (fp->name);
	}
	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VL_LOG_FILE_OPEN_SUBFILE);

	// Open the LOG group
	loc.obj_type = H5I_FILE;
	loc.type	 = H5VL_OBJECT_BY_SELF;
	H5VL_LOGI_PROFILING_TIMER_START
	fp->lgp = H5VLgroup_open (fp->sfp, &loc, fp->uvlid, LOG_GROUP_NAME, H5P_GROUP_ACCESS_DEFAULT,
							  fp->dxplid, NULL);
	CHECK_PTR (fp->lgp)
	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VLGROUP_OPEN);

	// Open the file with MPI
	H5VL_LOGI_PROFILING_TIMER_START;
	mpierr =
		MPI_File_open (fp->group_comm, fp->subname.c_str (), MPI_MODE_RDWR, fp->info, &(fp->fh));

	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VL_LOG_FILE_CREATE_FH);
	CHECK_MPIERR

	// Visit all dataasets for info
	args.op_type			 = H5VL_OBJECT_VISIT;
	args.args.visit.idx_type = H5_INDEX_CRT_ORDER;
	args.args.visit.order	 = H5_ITER_INC;
	args.args.visit.op		 = H5VL_log_filei_dset_visit;
	args.args.visit.op_data	 = NULL;
	args.args.visit.fields	 = H5O_INFO_ALL;
	err = H5VLobject_specific (fp->uo, &loc, fp->uvlid, &args, fp->dxplid, NULL);
	CHECK_ERR

	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VL_LOG_FILE_OPEN);

err_out:;
	return err;
}

herr_t H5VL_log_filei_dset_visit (hid_t o_id,
								  const char *name,
								  const H5O_info_t *object_info,
								  void *op_data) {
	herr_t err = 0;
	hid_t did  = -1;  // Target dataset ID

	// Skip unnamed and hidden object
	if ((name == NULL) || (name[0] == '_') || (name[0] == '/' || (name[0] == '.'))) {
		goto err_out;
	}

	// Open the dataset so that the dset metadata is cached in the file
	if (object_info->type == H5O_TYPE_DATASET) {
		did = H5Dopen2 (o_id, name, H5P_DEFAULT);
		CHECK_ID (did)
		H5Dclose (did);
	}

err_out:;
	return err;
}

herr_t H5VL_log_filei_bfree (H5VL_log_file_t *fp, void *buf) {
	size_t *bp;

	if (buf) {
		// The first sizeof (size_t) bytes are used to store the size of the buffer allocated, so
		// the real buffer to free is sizeof (size_t) bytes before buf
		bp = ((size_t *)buf) - 1;
		fp->bused -= bp[0];
		free (bp);
		return 0;
	}

	return -1;
}

herr_t H5VL_log_filei_parse_fapl (H5VL_log_file_t *fp, hid_t faplid) {
	herr_t err = 0;
	hbool_t ret;
	H5VL_log_sel_encoding_t encoding;
	char *env;

	err = H5Pget_meta_merge (faplid, &ret);
	CHECK_ERR
	if (ret) { fp->config |= H5VL_FILEI_CONFIG_METADATA_MERGE; }
	env = getenv ("H5VL_LOG_METADATA_MERGE");
	if (env) {
		if (strcmp (env, "1") == 0) {
			fp->config |= H5VL_FILEI_CONFIG_METADATA_MERGE;
		} else {
			fp->config &= ~H5VL_FILEI_CONFIG_METADATA_MERGE;
		}
	}

	err = H5Pget_meta_share (faplid, &ret);
	CHECK_ERR
	if (ret) { fp->config |= H5VL_FILEI_CONFIG_METADATA_SHARE; }
	env = getenv ("H5VL_LOG_METADATA_SHARE");
	if (env) {
		if (strcmp (env, "1") == 0) {
			fp->config |= H5VL_FILEI_CONFIG_METADATA_SHARE;
		} else {
			fp->config &= ~H5VL_FILEI_CONFIG_METADATA_SHARE;
		}
	}

	err = H5Pget_meta_zip (faplid, &ret);
	CHECK_ERR
	if (ret) { fp->config |= H5VL_FILEI_CONFIG_SEL_DEFLATE; }
	env = getenv ("H5VL_LOG_METADATA_ZIP");
	if (env) {
		if (strcmp (env, "1") == 0) {
			fp->config |= H5VL_FILEI_CONFIG_SEL_DEFLATE;
		} else {
			fp->config &= ~H5VL_FILEI_CONFIG_SEL_DEFLATE;
		}
	}

	err = H5Pget_sel_encoding (faplid, &encoding);
	CHECK_ERR
	if (encoding == H5VL_LOG_ENCODING_OFFSET) { fp->config |= H5VL_FILEI_CONFIG_SEL_ENCODE; }
	env = getenv ("H5VL_LOG_SEL_ENCODING");
	if (env) {
		if (strcmp (env, "canonical") == 0) {
			fp->config &= ~H5VL_FILEI_CONFIG_SEL_ENCODE;
		} else {
			fp->config |= H5VL_FILEI_CONFIG_SEL_ENCODE;
		}
	}

	err = H5Pget_idx_buffer_size (faplid, &(fp->mbuf_size));
	CHECK_ERR
	env = getenv ("H5VL_LOG_IDX_BSIZE");
	if (env) { fp->mbuf_size = (MPI_Offset) (atoll (env)); }

err_out:;
	return err;
}

herr_t H5VL_log_filei_parse_fcpl (H5VL_log_file_t *fp, hid_t fcplid) {
	herr_t err = 0;
	H5VL_log_data_layout_t layout;
	hbool_t subfiling;
	char *env;

	err = H5Pget_data_layout (fcplid, &layout);
	CHECK_ERR
	if (layout == H5VL_LOG_DATA_LAYOUT_CHUNK_ALIGNED) {
		fp->config |= H5VL_FILEI_CONFIG_DATA_ALIGN;
	}

	err = H5Pget_subfiling (fcplid, &subfiling);
	CHECK_ERR
	if (subfiling == true) { fp->config |= H5VL_FILEI_CONFIG_SUBFILING; }

	env = getenv ("H5VL_LOG_DATA_LAYOUT");
	if (env) {
		if (strcmp (env, "align") == 0) {
			fp->config &= ~H5VL_FILEI_CONFIG_DATA_ALIGN;

		} else {
			fp->config |= H5VL_FILEI_CONFIG_DATA_ALIGN;
		}
	}

	env = getenv ("H5VL_LOG_SUBFILING");
	if (env) {
		if (strcmp (env, "1") == 0) {
			fp->config |= H5VL_FILEI_CONFIG_SUBFILING;
		} else {
			fp->config &= ~H5VL_FILEI_CONFIG_SUBFILING;
		}
	}

	if (fp->config & H5VL_FILEI_CONFIG_SUBFILING) {
		env = getenv ("H5VL_LOG_N_SUBFILE");
		if (env) {
			fp->ngroup = atoi (env);
		} else {
			fp->ngroup = 0;
		}
	}

err_out:;
	return err;
}

H5VL_log_buffer_block_t *H5VL_log_filei_pool_new_block (size_t bsize) {
	// herr_t err					= 0;
	H5VL_log_buffer_block_t *bp = NULL;

	assert (bsize > 0);

	bp = (H5VL_log_buffer_block_t *)malloc (sizeof (H5VL_log_buffer_block_t));
	CHECK_PTR (bp)

	bp->cur = bp->begin = (char *)malloc (bsize);
	CHECK_PTR (bp->begin);
	bp->end = bp->begin + bsize;

	bp->next = NULL;

err_out:;
	if (!(bp->begin)) {
		free (bp);
		bp = NULL;
	}
	return bp;
}

herr_t H5VL_log_filei_pool_alloc (H5VL_log_buffer_pool_t *p, ssize_t bsize, void **buf) {
	herr_t err = 0;
	H5VL_log_buffer_block_t *bp;

	// Need to add blocks
	if (p->head->cur + bsize > p->head->end) {
		if (!(p->inf)) {
			err = -1;
			ERR_OUT ("Out of buffer")
		}

		if (bsize > p->bsize) {
			bp = H5VL_log_filei_pool_new_block (bsize);
		} else {
			if (p->free_blocks) {  // pull from recycle list
				bp			   = p->free_blocks;
				p->free_blocks = bp->next;
			} else {
				bp = H5VL_log_filei_pool_new_block ((size_t) (p->bsize));
			}
		}
		if (!bp) err = -1;
		CHECK_PTR (bp)

		bp->next = p->head;
		p->head	 = bp;
	}

	*buf = p->head->cur;
	p->head->cur += bsize;

err_out:;
	return err;
}

herr_t H5VL_log_filei_pool_init (H5VL_log_buffer_pool_t *p, ssize_t bsize) {
	herr_t err = 0;

	if (bsize < 0) {
		p->bsize = DEFAULT_SIZE;
		p->inf	 = 1;
	} else {
		p->bsize = bsize;
		p->inf	 = 0;
	}

	if (p->bsize) {
		p->head = H5VL_log_filei_pool_new_block ((size_t) (p->bsize));
		CHECK_PTR (p->head);
	} else {
		p->head = NULL;
	}
	p->free_blocks = NULL;

err_out:;
	return err;
}

herr_t H5VL_log_filei_pool_free (H5VL_log_buffer_pool_t *p) {
	herr_t err = 0;
	H5VL_log_buffer_block_t *i, *j = NULL;

	for (i = p->head->next; i; i = i->next) {
		i->cur = i->begin;
		j	   = i;
	}

	if (j) {
		j->next		   = p->free_blocks;
		p->free_blocks = p->head->next;
		p->head->next  = NULL;
	}
	p->head->cur = p->head->begin;

	// err_out:;
	return err;
}

herr_t H5VL_log_filei_pool_finalize (H5VL_log_buffer_pool_t *p) {
	herr_t err = 0;
	H5VL_log_buffer_block_t *i, *j;

	for (i = p->head; i; i = j) {
		j = i->next;
		free (i->begin);
		free (i);
	}
	p->head = NULL;
	for (i = p->free_blocks; i; i = j) {
		j = i->next;
		free (i->begin);
		free (i);
	}
	p->free_blocks = NULL;

	p->bsize = 0;
	p->inf	 = 0;
	// err_out:;
	return err;
}

herr_t H5VL_log_filei_contig_buffer_init (H5VL_log_contig_buffer_t *bp, size_t init_size) {
	herr_t err = 0;

	bp->begin = (char *)malloc (init_size);
	CHECK_PTR (bp->begin);

	bp->cur = bp->begin;
	bp->end = bp->begin + init_size;

err_out:;
	return err;
}

void H5VL_log_filei_contig_buffer_free (H5VL_log_contig_buffer_t *bp) { free (bp->begin); }

void *H5VL_log_filei_contig_buffer_alloc (H5VL_log_contig_buffer_t *bp, size_t size) {
	char *tmp;

	if (bp->cur + size > bp->end) {
		size_t new_size = bp->end - bp->begin;
		size_t used		= bp->end - bp->cur;

		while (used + size > new_size) new_size <<= 1;

		tmp = (char *)realloc (bp->begin, new_size);
		if (!tmp) return NULL;

		bp->begin = tmp;
		bp->cur	  = bp->begin + used;
		bp->end	  = bp->begin + new_size;
	}

	tmp = bp->cur;
	bp->cur += size;

	return (void *)tmp;
}

herr_t H5VL_log_filei_contig_buffer_alloc (H5VL_log_buffer_pool_t *p) {
	herr_t err = 0;
	H5VL_log_buffer_block_t *i, *j;

	for (i = p->head; i; i = j) {
		j = i->next;
		free (i->begin);
		free (i);
	}
	p->head = NULL;
	for (i = p->free_blocks; i; i = j) {
		j = i->next;
		free (i->begin);
		free (i);
	}
	p->free_blocks = NULL;

	p->bsize = 0;
	p->inf	 = 0;
	// err_out:;
	return err;
}

herr_t H5VL_log_filei_flush (H5VL_log_file_t *fp, hid_t dxplid) {
	herr_t err = 0;
	H5VL_LOGI_PROFILING_TIMER_START;

	if (fp->wreqs.size () > 0) {
		if (fp->config & H5VL_FILEI_CONFIG_DATA_ALIGN) {
			err = H5VL_log_nb_flush_write_reqs_align (fp, dxplid);
		} else {
			err = H5VL_log_nb_flush_write_reqs (fp, dxplid);
		}
		CHECK_ERR
	}

	if (fp->rreqs.size () > 0) {
		err = H5VL_log_nb_flush_read_reqs (fp, fp->rreqs, dxplid);
		CHECK_ERR
		fp->rreqs.clear ();
	}

	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VL_LOG_FILEI_FLUSH);
err_out:;
	return err;
}

static inline void print_info (MPI_Info *info_used) {
	int i, nkeys;

	if (*info_used == MPI_INFO_NULL) {
		printf ("MPI File Info is NULL\n");
		return;
	}
	MPI_Info_get_nkeys (*info_used, &nkeys);
	printf ("MPI File Info: nkeys = %d\n", nkeys);
	for (i = 0; i < nkeys; i++) {
		char key[MPI_MAX_INFO_KEY], value[MPI_MAX_INFO_VAL];
		int valuelen, flag;

		MPI_Info_get_nthkey (*info_used, i, key);
		MPI_Info_get_valuelen (*info_used, key, &valuelen, &flag);
		MPI_Info_get (*info_used, key, valuelen + 1, value, &flag);
		printf ("MPI File Info: [%2d] key = %25s, value = %s\n", i, key, value);
	}
	printf ("-----------------------------------------------------------\n");
}

herr_t H5VL_log_filei_close (H5VL_log_file_t *fp) {
	herr_t err = 0;
	int mpierr;
	int attbuf[5];

	H5VL_LOGI_PROFILING_TIMER_START;

#ifdef LOGVOL_DEBUG
	if (H5VL_logi_debug_verbose ()) { printf ("H5VL_log_filei_close(%p, ...)\n", fp); }
#endif

	if (fp->flag != H5F_ACC_RDONLY) {
		// Flush write requests
		if ((int)(fp->wreqs.size ()) > fp->nflushed) {
			if (fp->config & H5VL_FILEI_CONFIG_DATA_ALIGN) {
				err = H5VL_log_nb_flush_write_reqs_align (fp, fp->dxplid);
			} else {
				err = H5VL_log_nb_flush_write_reqs (fp, fp->dxplid);
			}
			CHECK_ERR
		}

		// Generate metadata table
		err = H5VL_log_filei_metaflush (fp);
		CHECK_ERR

		// Update file attr
		attbuf[0] = fp->ndset;
		attbuf[1] = fp->nldset;
		attbuf[2] = fp->nmdset;
		attbuf[3] = fp->config;
		attbuf[4] = fp->ngroup;
		// Att in the subfile
		if (fp->sfp && fp->sfp != fp->uo) {
			attbuf[3] =
				fp->config & !(H5VL_FILEI_CONFIG_SUBFILING);  // No subfiling flag in a subfile
			err = H5VL_logi_put_att (fp->sfp, fp->uvlid, H5I_FILE, "_int_att", H5T_NATIVE_INT32,
									 attbuf, fp->dxplid);
			attbuf[1] = 0;	// No data and metadata in the main file
			attbuf[2] = 0;
			attbuf[3] |= H5VL_FILEI_CONFIG_SUBFILING;  // Turn subfiling flag back on
		}
		// Att in the main file
		err = H5VL_logi_put_att (fp, "_int_att", H5T_NATIVE_INT32, attbuf, fp->dxplid);
		CHECK_ERR
	}

	// Close the log group
	H5VL_LOGI_PROFILING_TIMER_START
	err = H5VLgroup_close (fp->lgp, fp->uvlid, fp->dxplid, NULL);
	CHECK_ERR
	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VLGROUP_CLOSE);

#ifdef LOGVOL_PROFILING
	{
		MPI_Info info;
		char *_env_str = getenv ("H5VL_LOG_PRINT_MPI_INFO");
		if (_env_str != NULL && *_env_str != '0') {
			if (fp->rank == 0) {
				MPI_File_get_info (fp->fh, &info);
				print_info (&info);
				MPI_Info_free (&info);
			}
		}
	}
#endif

	// Close the file with MPI
	mpierr = MPI_File_close (&(fp->fh));
	CHECK_MPIERR

	// Close the file with posix
	if (fp->config & H5VL_FILEI_CONFIG_DATA_ALIGN) { close (fp->fd); }

	// Close contig dataspacce ID
	H5VL_log_dataspace_contig_ref--;
	if (H5VL_log_dataspace_contig_ref == 0) { H5Sclose (H5VL_log_dataspace_contig); }

	// Free compression buffer
	free (fp->zbuf);

	// Close the file with under VOL
	H5VL_LOGI_PROFILING_TIMER_START;
	err = H5VLfile_close (fp->uo, fp->uvlid, H5P_DATASET_XFER_DEFAULT, NULL);
	CHECK_ERR
	if (fp->sfp && (fp->sfp != fp->uo)) {
		err = H5VLfile_close (fp->sfp, fp->uvlid, H5P_DATASET_XFER_DEFAULT, NULL);
		CHECK_ERR
	}
	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VLFILE_CLOSE);

	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VL_LOG_FILE_CLOSE);

#ifdef LOGVOL_PROFILING
	{
		char *_env_str = getenv ("H5VL_LOG_SHOW_PROFILING_INFO");
		if (_env_str != NULL && *_env_str != '0') { H5VL_log_profile_print (fp); }
	}
#endif

	// Clean up
	if (fp->group_comm != fp->comm) { MPI_Comm_free (&(fp->group_comm)); }
	MPI_Comm_free (&(fp->comm));
	if (fp->info != MPI_INFO_NULL) { MPI_Info_free (&(fp->info)); }
	H5Pclose (fp->dxplid);
	H5Pclose (fp->ufaplid);

	delete fp;

err_out:
	return err;
} /* end H5VL_log_file_close() */

herr_t H5VL_log_filei_parse_strip_info (H5VL_log_file_t *fp) {
	herr_t err	  = 0;
	MPI_Info info = MPI_INFO_NULL;
	int mpierr;
	int exist;
	char val[128];

	mpierr = MPI_File_get_info (fp->fh, &info);
	CHECK_MPIERR

	fp->scount = -1;
	fp->ssize  = -1;

	mpierr = MPI_Info_get (info, "striping_factor", 128, val, &exist);
	CHECK_MPIERR
	if (exist) { fp->scount = atoi (val); }
	mpierr = MPI_Info_get (info, "striping_unit", 128, val, &exist);
	CHECK_MPIERR
	if (exist) { fp->ssize = (size_t)atoll (val); }

err_out:
	if (info != MPI_INFO_NULL) MPI_Info_free (&info);
	return err;
}

herr_t H5VL_log_filei_create_subfile (H5VL_log_file_t *fp,
									  unsigned flags,
									  hid_t fapl_id,
									  hid_t dxpl_id) {
	herr_t err = 0;
	int attbuf[5];
	int stat;

	// Create subfile dir
	if (fp->rank == 0) {
		stat = mkdir ((fp->name + ".subfiles").c_str (), S_IRWXU);
		// Skip creation if already exist
		if (stat == -1 && errno == EEXIST) stat = 0;
	}
	MPI_Bcast (&stat, 1, MPI_INT, 0, fp->comm);
	if (stat != 0) { RET_ERR ("Cannot create subfile dir") }

	// Create the subfiles with underlying VOL
	err = H5Pset_fapl_mpio (fapl_id, fp->group_comm, MPI_INFO_NULL);
	CHECK_ERR
	H5VL_LOGI_PROFILING_TIMER_START;
	fp->subname = fp->name + ".subfiles/" + std::to_string (fp->group_id) + ".h5";
	fp->sfp		= H5VLfile_create (fp->subname.c_str (), flags, H5P_FILE_CREATE_DEFAULT, fapl_id,
							   dxpl_id, NULL);
	CHECK_PTR (fp->sfp)
	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VLFILE_CREATE);

	// Att
	attbuf[0] = fp->ndset;
	attbuf[1] = fp->nldset;
	attbuf[2] = fp->nmdset;
	attbuf[3] = fp->config & !(H5VL_FILEI_CONFIG_SUBFILING);  // No subfiling flag in a subfile
	attbuf[4] = fp->ngroup;
	err		  = H5VL_logi_add_att (fp->sfp, fp->uvlid, H5I_FILE, "_int_att", H5T_STD_I32LE,
							   H5T_NATIVE_INT32, 4, attbuf, dxpl_id, NULL);
	CHECK_ERR

err_out:;
	return err;
}

herr_t H5VL_log_filei_open_subfile (H5VL_log_file_t *fp,
									unsigned flags,
									hid_t fapl_id,
									hid_t dxpl_id) {
	herr_t err = 0;
	int attbuf[5];
	int stat;

	// Open subfile dir
	if (fp->rank == 0) {
		DIR *dir;
		dir = opendir ((fp->name + ".subfiles").c_str ());
		if (dir) {
			closedir (dir);
			stat = 0;
		} else {
			stat = -1;
		}
	}
	MPI_Bcast (&stat, 1, MPI_INT, 0, fp->comm);
	if (stat != 0) { RET_ERR ("Cannot open subfile dir") }

	// Create the subfiles with underlying VOL
	err = H5Pset_fapl_mpio (fapl_id, fp->group_comm, MPI_INFO_NULL);
	CHECK_ERR
	H5VL_LOGI_PROFILING_TIMER_START;
	fp->subname = fp->name + ".subfiles/" + std::to_string (fp->group_id) + ".h5";
	fp->sfp		= H5VLfile_open (fp->subname.c_str (), flags, fapl_id, dxpl_id, NULL);
	CHECK_PTR (fp->sfp)
	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VLFILE_CREATE);

	// Att
	err = H5VL_logi_get_att (fp, "_int_att", H5T_NATIVE_INT32, attbuf, fp->dxplid);
	CHECK_ERR
	fp->nldset = attbuf[1];
	fp->nmdset = attbuf[2];

err_out:;
	return err;
}

herr_t H5VL_log_filei_calc_node_rank (H5VL_log_file_t *fp) {
	herr_t err = 0;
	int mpierr;
	int i, j;
	MPI_Info info	 = MPI_INFO_NULL;
	int *group_ranks = NULL;  // Rank of the group (processes sharing a subfile)

	group_ranks = (int *)malloc (sizeof (int) * fp->np);
	CHECK_PTR (group_ranks);

	if (fp->ngroup > 0) {
		mpierr =
			MPI_Comm_split (fp->comm, fp->rank * fp->ngroup / fp->np, fp->rank, &(fp->group_comm));
	} else {
		mpierr = MPI_Comm_split_type (fp->comm, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL,
									  &(fp->group_comm));
	}
	CHECK_ERR

	mpierr = MPI_Comm_rank (fp->group_comm, &(fp->group_rank));
	CHECK_MPIERR
	mpierr = MPI_Comm_size (fp->group_comm, &(fp->group_np));
	CHECK_MPIERR

	mpierr = MPI_Allgather (&(fp->group_rank), 1, MPI_INT, group_ranks, 1, MPI_INT, fp->comm);
	CHECK_MPIERR
	// Assign group ID based on the global rank of group rank 0
	fp->group_id = 0;
	for (i = 0; i < fp->rank; i++) {
		if (group_ranks[i] == 0) { fp->group_id++; }
	}
	mpierr = MPI_Bcast (&(fp->group_id), 1, MPI_INT, 0, fp->group_comm);
	CHECK_MPIERR

	if (fp->config & H5VL_FILEI_CONFIG_DATA_ALIGN) {
		// What ost it should write to
		fp->target_ost = fp->group_id % fp->scount;

		// Find previous and next node sharing the ost
		fp->prev_rank = -1;
		fp->next_rank = -1;
		j			  = fp->scount;
		for (i = fp->rank - 1; i > -1; i--) {
			if (group_ranks[i] == 0) {
				j--;
				if (j == 0) {
					fp->prev_rank = i;
					break;
				}
			}
		}
		j = fp->scount;
		for (i = fp->rank + 1; i < fp->np; i++) {
			if (group_ranks[i] == 0) {
				j--;
				if (j == 0) {
					fp->next_rank = i;
					break;
				}
			}
		}
	}

err_out:
	if (info != MPI_INFO_NULL) MPI_Info_free (&info);
	H5VL_log_free (group_ranks);
	return err;
}

/*-------------------------------------------------------------------------
 * Function:    H5VL_log_dataset_open
 *
 * Purpose:     Opens a dataset in a container
 *
 * Return:      Success:    Pointer to a dataset object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
void *H5VL_log_filei_wrap (void *uo, H5VL_log_obj_t *cp) {
	// herr_t err = 0;
	// int mpierr;
	H5VL_log_file_t *fp = NULL;
	// H5VL_loc_params_t loc;
	// int attbuf[3];
	H5VL_LOGI_PROFILING_TIMER_START;

	/*
		fp = new H5VL_log_file_t (uo, cp->uvlid);
		CHECK_PTR (fp)
		fp->flag = cp->fp->flag;
		MPI_Comm_dup (cp->fp->comm, &(fp->comm));
		fp->rank   = cp->fp->rank;
		fp->dxplid = H5Pcopy (cp->fp->dxplid);
		fp->bsize  = cp->fp->bsize;
		fp->name   = cp->fp->name;

		// err=H5VL_log_filei_pool_init(&(fp->data_buf),fp->bsize);
		// CHECK_ERR
		err = H5VL_log_filei_contig_buffer_init (&(fp->meta_buf), 2097152);	 // 200 MiB
		CHECK_ERR

		// Create LOG group
		loc.obj_type = H5I_FILE;
		loc.type	 = H5VL_OBJECT_BY_SELF;
		H5VL_LOGI_PROFILING_TIMER_START
		fp->lgp = H5VLgroup_open (fp->uo, &loc, fp->uvlid, LOG_GROUP_NAME, H5P_GROUP_ACCESS_DEFAULT,
								  fp->dxplid, NULL);
		CHECK_PTR (fp->lgp)
		H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VLGROUP_OPEN);

		// Att
		err = H5VL_logi_get_att (fp, "_int_att", H5T_NATIVE_INT32, attbuf, fp->dxplid);
		CHECK_ERR
		fp->ndset  = attbuf[0];
		fp->nldset = attbuf[1];
		fp->nmdset = attbuf[2];
		fp->idx.resize (fp->ndset);

		// Open the file with MPI
		mpierr = MPI_File_open (fp->comm, fp->name.c_str (), MPI_MODE_RDWR, MPI_INFO_NULL,
	   &(fp->fh)); CHECK_MPIERR
	*/

	fp = cp->fp;
	// fp->refcnt++;

	H5VL_LOGI_PROFILING_TIMER_STOP (fp, TIMER_H5VL_LOG_FILE_OPEN);

	goto fn_exit;
	// err_out:;
	if (fp) { delete fp; }
	fp = NULL;
fn_exit:;
	return (void *)fp;
} /* end H5VL_log_dataset_open() */

void H5VL_log_filei_inc_ref (H5VL_log_file_t *fp) { fp->refcnt++; }

herr_t H5VL_log_filei_dec_ref (H5VL_log_file_t *fp) {
	fp->refcnt--;
	if (fp->refcnt == 0) { return H5VL_log_filei_close (fp); }
	return 0;
}

H5VL_log_file_t::H5VL_log_file_t () {
	this->refcnt	= 1;
	this->closing	= false;
	this->fp		= this;
	this->type		= H5I_FILE;
	this->nflushed	= 0;
	this->type		= H5I_FILE;
	this->idxvalid	= false;
	this->metadirty = false;
#ifdef LOGVOL_DEBUG
	this->ext_ref = 0;
#endif
#ifdef LOGVOL_PROFILING
	H5VL_log_profile_reset (fp);
#endif
}
H5VL_log_file_t::H5VL_log_file_t (hid_t uvlid) : H5VL_log_file_t () {
	this->uvlid = uvlid;
#ifdef LOGVOL_DEBUG
	this->ext_ref = 1;
#endif
	H5VL_logi_inc_ref (this->uvlid);
}
H5VL_log_file_t::H5VL_log_file_t (void *uo, hid_t uvlid) : H5VL_log_file_t (uvlid) {
	this->uo = uo;
}
/*
H5VL_log_file_t::~H5VL_log_file_t () {
#ifdef LOGVOL_DEBUG
	this->ext_ref--;
	assert (this->ext_ref >= 0);
#endif
	H5VL_logi_dec_ref (this->uvlid);
}
*/