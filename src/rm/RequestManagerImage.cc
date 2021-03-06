/* -------------------------------------------------------------------------- */
/* Copyright 2002-2013, OpenNebula Project (OpenNebula.org), C12G Labs        */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0 (the "License"); you may    */
/* not use this file except in compliance with the License. You may obtain    */
/* a copy of the License at                                                   */
/*                                                                            */
/* http://www.apache.org/licenses/LICENSE-2.0                                 */
/*                                                                            */
/* Unless required by applicable law or agreed to in writing, software        */
/* distributed under the License is distributed on an "AS IS" BASIS,          */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   */
/* See the License for the specific language governing permissions and        */
/* limitations under the License.                                             */
/* -------------------------------------------------------------------------- */

#include "RequestManagerImage.h"

using namespace std;

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

void ImageEnable::request_execute(xmlrpc_c::paramList const& paramList,
                                  RequestAttributes& att)
{
    int     id          = xmlrpc_c::value_int(paramList.getInt(1));
    bool    enable_flag = xmlrpc_c::value_boolean(paramList.getBoolean(2));
    int     rc;

    string err_msg;

    Nebula&          nd     = Nebula::instance();
    ImageManager *   imagem = nd.get_imagem();

    if ( basic_authorization(id, att) == false )
    {
        return;
    }

    rc = imagem->enable_image(id,enable_flag, err_msg);

    if( rc < 0 )
    {
        if (enable_flag == true)
        {
            err_msg = "Could not enable image: " + err_msg;
        }
        else
        {
            err_msg = "Could not disable image: " + err_msg;
        }

        failure_response(INTERNAL, request_error(err_msg,""), att);
        return;
    }

    success_response(id, att);
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

void ImagePersistent::request_execute(xmlrpc_c::paramList const& paramList,
                                      RequestAttributes& att)
{
    int     id              = xmlrpc_c::value_int(paramList.getInt(1));
    bool    persistent_flag = xmlrpc_c::value_boolean(paramList.getBoolean(2));
    int     rc;

    Image * image;
    string  err_msg;

    if ( basic_authorization(id, att) == false )
    {
        return;
    }

    image = static_cast<Image *>(pool->get(id,true));

    if ( image == 0 )
    {
        failure_response(NO_EXISTS,
                get_error(object_name(auth_object),id),
                att);

        return;
    }

    switch (image->get_type())
    {
        case Image::OS:
        case Image::DATABLOCK:
        case Image::CDROM:
        break;

        case Image::KERNEL:
        case Image::RAMDISK:
        case Image::CONTEXT:
            failure_response(ACTION,
                request_error("KERNEL, RAMDISK and CONTEXT files must be "
                "non-persistent",""), att);
            image->unlock();
        return;
    }

    rc = image->persistent(persistent_flag, err_msg);

    if ( rc != 0  )
    {
        if (persistent_flag == true)
        {
            err_msg = "Could not make image persistent: " + err_msg;
        }
        else
        {
            err_msg = "Could not make image non-persistent: " + err_msg;
        }

        failure_response(INTERNAL,request_error(err_msg,""), att);

        image->unlock();
        return;
    }

    pool->update(image);

    image->unlock();

    success_response(id, att);
}

/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */

void ImageChangeType::request_execute(xmlrpc_c::paramList const& paramList,
                                      RequestAttributes& att)
{
    int     id   = xmlrpc_c::value_int(paramList.getInt(1));
    string  type = xmlrpc_c::value_string(paramList.getString(2));
    int     rc;

    Image::ImageType itype;

    Image * image;
    string  err_msg;

    if ( basic_authorization(id, att) == false )
    {
        return;
    }

    image = static_cast<Image *>(pool->get(id,true));

    if ( image == 0 )
    {
        failure_response(NO_EXISTS,
                get_error(object_name(auth_object),id),
                att);

        return;
    }

    itype = Image::str_to_type(type);

    switch (image->get_type())
    {
        case Image::OS:
        case Image::DATABLOCK:
        case Image::CDROM:
            if ((itype != Image::OS) &&
                (itype != Image::DATABLOCK)&&
                (itype != Image::CDROM) )
            {
                failure_response(ACTION,
                    request_error("Cannot change image type to an incompatible"
                        " type for the current datastore.",""),
                    att);

                image->unlock();
                return;
            }
        break;

        case Image::KERNEL:
        case Image::RAMDISK:
        case Image::CONTEXT:
            if ((itype != Image::KERNEL) &&
                (itype != Image::RAMDISK)&&
                (itype != Image::CONTEXT) )
            {
                failure_response(ACTION,
                    request_error("Cannot change image type to an incompatible"
                        " type for the current datastore.",""),
                    att);

                image->unlock();
                return;
            }
        break;
    }

    rc = image->set_type(type);

    if ( rc != 0  )
    {
        err_msg = "Unknown type " + type;

        failure_response(INTERNAL,request_error(err_msg,""), att);

        image->unlock();
        return;
    }

    pool->update(image);

    image->unlock();

    success_response(id, att);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void ImageClone::request_execute(
        xmlrpc_c::paramList const&  paramList,
        RequestAttributes&          att)
{
    int    clone_id = xmlrpc_c::value_int(paramList.getInt(1));
    string name     = xmlrpc_c::value_string(paramList.getString(2));

    unsigned int    avail;
    int             rc, new_id, ds_id, size, umask;
    string          error_str, ds_name, ds_data;
    bool            ds_check;

    Image::DiskType disk_type;
    PoolObjectAuth  perms, ds_perms;

    ImageTemplate * tmpl;
    Template        img_usage;
    Image *         img;
    Datastore *     ds;
    User *          user;

    Nebula&  nd = Nebula::instance();

    DatastorePool * dspool = nd.get_dspool();
    ImagePool *     ipool  = static_cast<ImagePool *>(pool);
    UserPool *      upool  = nd.get_upool();

    // ------------------------- Get user's umask ------------------------------

    user = upool->get(att.uid, true);

    if ( user == 0 )
    {
        failure_response(NO_EXISTS,
                get_error(object_name(PoolObjectSQL::USER), att.uid),
                att);

        return;
    }

    umask = user->get_umask();

    user->unlock();

    // ------------------------- Get source Image info -------------------------

    img = ipool->get(clone_id, true);

    if ( img == 0 )
    {
        failure_response(NO_EXISTS,
                get_error(object_name(auth_object), clone_id),
                att);

        return;
    }

    switch (img->get_type())
    {
        case Image::OS:
        case Image::DATABLOCK:
        case Image::CDROM:
        break;

        case Image::KERNEL:
        case Image::RAMDISK:
        case Image::CONTEXT:
            failure_response(ACTION,
                allocate_error("KERNEL, RAMDISK and CONTEXT files cannot be "
                    "cloned."), att);
            img->unlock();
        return;
    }

    tmpl = img->clone_template(name);

    img->get_permissions(perms);

    ds_id   = img->get_ds_id();
    ds_name = img->get_ds_name();
    size    = img->get_size();

    img->unlock();

    // ------------------------- Get Datastore info ----------------------------

    ds = dspool->get(ds_id, true);

    if ( ds == 0 )
    {
        failure_response(NO_EXISTS,
                get_error(object_name(PoolObjectSQL::DATASTORE), ds_id),
                att);

        delete tmpl;
        return;
    }

    if ( ds->get_type() == Datastore::FILE_DS )
    {
        failure_response(ACTION, "Clone not supported for FILE_DS Datastores",
            att);

        delete tmpl;

        ds->unlock();

        return;
    }

    ds->get_permissions(ds_perms);

    disk_type = ds->get_disk_type();

    ds->to_xml(ds_data);

    ds_check = ds->get_avail_mb(avail);

    ds->unlock();

    // ------------- Set authorization request ---------------------------------

    img_usage.add("DATASTORE", ds_id);
    img_usage.add("SIZE", size);

    if (ds_check && ((unsigned int) size > avail))
    {
        failure_response(ACTION, "Not enough space in datastore", att);

        delete tmpl;
        return;
    }

    if ( att.uid != 0 )
    {
        AuthRequest ar(att.uid, att.gid);
        string      tmpl_str;

        // ------------------ Check permissions and ACLs  ----------------------

        tmpl->to_xml(tmpl_str);

        ar.add_create_auth(auth_object, tmpl_str); // CREATE IMAGE

        ar.add_auth(AuthRequest::USE, ds_perms); // USE DATASTORE

        if (UserPool::authorize(ar) == -1)
        {
            failure_response(AUTHORIZATION,
                    authorization_error(ar.message, att),
                    att);

            delete tmpl;
            return;
        }

        // -------------------------- Check Quotas  ----------------------------

        if ( quota_authorization(&img_usage, Quotas::DATASTORE, att) == false )
        {
            delete tmpl;
            return;
        }
    }

    rc = ipool->allocate(att.uid,
                         att.gid,
                         att.uname,
                         att.gname,
                         umask,
                         tmpl,
                         ds_id,
                         ds_name,
                         disk_type,
                         ds_data,
                         Datastore::IMAGE_DS,
                         clone_id,
                         &new_id,
                         error_str);
    if ( rc < 0 )
    {
        quota_rollback(&img_usage, Quotas::DATASTORE, att);

        failure_response(INTERNAL, allocate_error(error_str), att);
        return;
    }

    ds = dspool->get(ds_id, true);

    if ( ds != 0 )  // TODO: error otherwise or leave image in ERROR?
    {
        ds->add_image(new_id);

        dspool->update(ds);

        ds->unlock();
    }

    success_response(new_id, att);
}


