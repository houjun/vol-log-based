#include "logvol_internal.hpp"

extern hid_t H5VL_LOG_g;

/*-------------------------------------------------------------------------
 * Function:    H5VL__H5VL_log_new_obj
 *
 * Purpose:     Create a new log object for an underlying object
 *
 * Return:      Success:    Pointer to the new log object
 *              Failure:    NULL
 *
 * Programmer:  Quincey Koziol
 *              Monday, December 3, 2018
 *
 *-------------------------------------------------------------------------
 */
H5VL_log_obj_t *H5VL_log_new_obj(void *under_obj, hid_t uvlid) {
    H5VL_log_obj_t *new_obj;

    new_obj = (H5VL_log_obj_t *)calloc(1, sizeof(H5VL_log_obj_t));
    new_obj->uo = under_obj;
    new_obj->uvlid = uvlid;
    //H5Iinc_ref(new_obj->uvlid);

    return new_obj;
} /* end H5VL__H5VL_log_new_obj() */




/*-------------------------------------------------------------------------
 * Function:    H5VL__H5VL_log_free_obj
 *
 * Purpose:     Release a log object
 *
 * Note:	Take care to preserve the current HDF5 error stack
 *		when calling HDF5 API calls.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 * Programmer:  Quincey Koziol
 *              Monday, December 3, 2018
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_log_free_obj(H5VL_log_obj_t *obj) {
    //hid_t err_id;

    //err_id = H5Eget_current_stack();

    //H5Idec_ref(obj->uvlid);

    //H5Eset_current_stack(err_id);

    free(obj);

    return 0;
} /* end H5VL__H5VL_log_free_obj() */


/*-------------------------------------------------------------------------
 * Function:    H5VL_log_register
 *
 * Purpose:     Register the pass-through VOL connector and retrieve an ID
 *              for it.
 *
 * Return:      Success:    The ID for the pass-through VOL connector
 *              Failure:    -1
 *
 * Programmer:  Quincey Koziol
 *              Wednesday, November 28, 2018
 *
 *-------------------------------------------------------------------------
 */
hid_t H5VL_log_register(void) {
    /* Singleton register the pass-through VOL connector ID */
    if(H5VL_LOG_g < 0)
        H5VL_LOG_g = H5VLregister_connector(&H5VL_log_g, H5P_DEFAULT);

    return H5VL_LOG_g;
} /* end H5VL_log_register() */


herr_t H5VLattr_get_wrapper(void *obj, hid_t connector_id, H5VL_attr_get_t get_type, hid_t dxpl_id, void **req, ...) {
    herr_t err;
    va_list args;

    va_start(args, req);
    err = H5VLattr_get(obj, connector_id, get_type, dxpl_id, req, args);
    va_end(args);

    return err;
}

herr_t H5VL_logi_add_att(H5VL_log_obj_t *op, char *name, hid_t atype, hid_t mtype, hsize_t len, void *buf, hid_t dxpl_id) {
    herr_t err = 0;
    H5VL_loc_params_t loc;
    hid_t asid = -1;
    void *ap;

    asid = H5Screate_simple(1, &len, &len); CHECK_ID(asid);

    loc.obj_type = op->type;
    loc.type = H5VL_OBJECT_BY_SELF;

    ap = H5VLattr_create(op->uo, &loc, op->uvlid, name, atype, asid, H5P_ATTRIBUTE_CREATE_DEFAULT, H5P_ATTRIBUTE_ACCESS_DEFAULT, dxpl_id, NULL); CHECK_NERR(ap);
    err = H5VLattr_write(ap, op->uvlid, mtype, buf, dxpl_id, NULL); CHECK_ERR;
    err = H5VLattr_close(ap, op->uvlid, dxpl_id, NULL); CHECK_ERR

    H5Sclose(asid);

err_out:;
    return err;
}

herr_t H5VL_logi_put_att(H5VL_log_obj_t *op, char *name, hid_t mtype, void *buf, hid_t dxpl_id) {
    herr_t err = 0;
    H5VL_loc_params_t loc;
    void *ap;

    loc.obj_type = op->type;
    loc.type = H5VL_OBJECT_BY_SELF;

    ap = H5VLattr_open(op->uo, &loc, op->uvlid, name, H5P_ATTRIBUTE_ACCESS_DEFAULT, dxpl_id, NULL); CHECK_NERR(ap);
    err = H5VLattr_write(ap, op->uvlid, mtype, buf, dxpl_id, NULL); CHECK_ERR;
    err = H5VLattr_close(ap, op->uvlid, dxpl_id, NULL); CHECK_ERR

err_out:;
    return err;
}

herr_t H5VL_logi_get_att(H5VL_log_obj_t *op, char *name, hid_t mtype, void *buf, hid_t dxpl_id) {
    herr_t err = 0;
    H5VL_loc_params_t loc;
    void *ap;

    loc.obj_type = op->type;
    loc.type = H5VL_OBJECT_BY_SELF;

    ap = H5VLattr_open(op->uo, &loc, op->uvlid, name, H5P_ATTRIBUTE_ACCESS_DEFAULT, dxpl_id, NULL); CHECK_NERR(ap);
    err = H5VLattr_read(ap, op->uvlid, mtype, buf, dxpl_id, NULL); CHECK_ERR;
    err = H5VLattr_close(ap, op->uvlid, dxpl_id, NULL); CHECK_ERR

err_out:;
    return err;
}