/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright 2019, Blender Foundation.
 */

/** \file
 * \ingroup draw_engine
 */

#include "DRW_render.h"

#include "BKE_camera.h"
#include "BKE_image.h"
#include "BKE_movieclip.h"
#include "BKE_object.h"

#include "DNA_camera_types.h"
#include "DNA_screen_types.h"

#include "DEG_depsgraph_query.h"

#include "ED_view3d.h"

#include "IMB_imbuf_types.h"

#include "overlay_private.h"

void OVERLAY_image_cache_init(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;
  OVERLAY_PrivateData *pd = vedata->stl->pd;

  DRWState state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA_UNDER_PREMUL;
  DRW_PASS_CREATE(psl->image_background_ps, state);

  state = DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_DEPTH_LESS;
  DRW_PASS_CREATE(psl->image_empties_ps, state | pd->clipping_state);

  state = DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_BLEND_ALPHA;
  DRW_PASS_CREATE(psl->image_empties_back_ps, state | pd->clipping_state);
  DRW_PASS_CREATE(psl->image_empties_blend_ps, state | pd->clipping_state);

  state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA;
  DRW_PASS_CREATE(psl->image_empties_front_ps, state);
  DRW_PASS_CREATE(psl->image_foreground_ps, state);
}

static void overlay_image_calc_aspect(Image *ima, const int size[2], float r_image_aspect[2])
{
  float ima_x, ima_y;
  if (ima) {
    ima_x = size[0];
    ima_y = size[1];
  }
  else {
    /* if no image, make it a 1x1 empty square, honor scale & offset */
    ima_x = ima_y = 1.0f;
  }
  /* Get the image aspect even if the buffer is invalid */
  float sca_x = 1.0f, sca_y = 1.0f;
  if (ima) {
    if (ima->aspx > ima->aspy) {
      sca_y = ima->aspy / ima->aspx;
    }
    else if (ima->aspx < ima->aspy) {
      sca_x = ima->aspx / ima->aspy;
    }
  }

  const float scale_x_inv = ima_x * sca_x;
  const float scale_y_inv = ima_y * sca_y;
  if (scale_x_inv > scale_y_inv) {
    r_image_aspect[0] = 1.0f;
    r_image_aspect[1] = scale_y_inv / scale_x_inv;
  }
  else {
    r_image_aspect[0] = scale_x_inv / scale_y_inv;
    r_image_aspect[1] = 1.0f;
  }
}

static void camera_background_images_stereo_setup(Scene *scene,
                                                  View3D *v3d,
                                                  Image *ima,
                                                  ImageUser *iuser)
{
  if (BKE_image_is_stereo(ima)) {
    iuser->flag |= IMA_SHOW_STEREO;

    if ((scene->r.scemode & R_MULTIVIEW) == 0) {
      iuser->multiview_eye = STEREO_LEFT_ID;
    }
    else if (v3d->stereo3d_camera != STEREO_3D_ID) {
      /* show only left or right camera */
      iuser->multiview_eye = v3d->stereo3d_camera;
    }

    BKE_image_multiview_index(ima, iuser);
  }
  else {
    iuser->flag &= ~IMA_SHOW_STEREO;
  }
}

static struct GPUTexture *image_camera_background_texture_get(CameraBGImage *bgpic,
                                                              const DRWContextState *draw_ctx,
                                                              OVERLAY_PrivateData *pd,
                                                              float *r_aspect,
                                                              bool *r_use_alpha_premult)
{
  Image *image = bgpic->ima;
  ImageUser *iuser = &bgpic->iuser;
  MovieClip *clip = NULL;
  GPUTexture *tex = NULL;
  Scene *scene = draw_ctx->scene;
  float aspect_x, aspect_y;
  int width, height;
  int ctime = (int)DEG_get_ctime(draw_ctx->depsgraph);
  *r_use_alpha_premult = false;

  switch (bgpic->source) {
    case CAM_BGIMG_SOURCE_IMAGE:
      if (image == NULL) {
        return NULL;
      }
      *r_use_alpha_premult = (image->alpha_mode == IMA_ALPHA_PREMUL);

      BKE_image_user_frame_calc(image, iuser, ctime);
      if (image->source == IMA_SRC_SEQUENCE && !(iuser->flag & IMA_USER_FRAME_IN_RANGE)) {
        /* Frame is out of range, dont show. */
        return NULL;
      }
      else {
        camera_background_images_stereo_setup(scene, draw_ctx->v3d, image, iuser);
      }

      ImBuf *ibuf = BKE_image_acquire_ibuf(image, iuser, NULL);
      if (ibuf == NULL) {
        return NULL;
      }

      tex = GPU_texture_from_blender(image, iuser, GL_TEXTURE_2D);
      if (tex == NULL) {
        return NULL;
      }

      aspect_x = bgpic->ima->aspx;
      aspect_y = bgpic->ima->aspy;

      width = ibuf->x;
      height = ibuf->y;

      BKE_image_release_ibuf(image, ibuf, NULL);
      break;

    case CAM_BGIMG_SOURCE_MOVIE:
      if (bgpic->flag & CAM_BGIMG_FLAG_CAMERACLIP) {
        if (scene->camera) {
          clip = BKE_object_movieclip_get(scene, scene->camera, true);
        }
      }
      else {
        clip = bgpic->clip;
      }

      if (clip == NULL) {
        return NULL;
      }

      BKE_movieclip_user_set_frame(&bgpic->cuser, ctime);
      tex = GPU_texture_from_movieclip(clip, &bgpic->cuser, GL_TEXTURE_2D);
      if (tex == NULL) {
        return NULL;
      }

      aspect_x = clip->aspx;
      aspect_y = clip->aspy;

      BKE_movieclip_get_size(clip, &bgpic->cuser, &width, &height);

      /* Save for freeing. */
      BLI_addtail(&pd->bg_movie_clips, BLI_genericNodeN(clip));
      break;

    default:
      /* Unsupported type. */
      return NULL;
  }

  *r_aspect = (width * aspect_x) / (height * aspect_y);
  return tex;
}

static void OVERLAY_image_free_movieclips_textures(OVERLAY_Data *data)
{
  /* Free Movie clip textures after rendering */
  LinkData *link;
  while ((link = BLI_pophead(&data->stl->pd->bg_movie_clips))) {
    MovieClip *clip = (MovieClip *)link->data;
    GPU_free_texture_movieclip(clip);
    MEM_freeN(link);
  }
}

static void image_camera_background_matrix_get(const Camera *cam,
                                               const CameraBGImage *bgpic,
                                               const DRWContextState *draw_ctx,
                                               const float image_aspect,
                                               float rmat[4][4])
{
  float rotate[4][4], scale[4][4], translate[4][4];

  axis_angle_to_mat4_single(rotate, 'Z', -bgpic->rotation);
  unit_m4(scale);
  unit_m4(translate);

  /*  Normalized Object space camera frame corners. */
  float cam_corners[4][3];
  BKE_camera_view_frame(draw_ctx->scene, cam, cam_corners);
  float cam_width = fabsf(cam_corners[0][0] - cam_corners[3][0]);
  float cam_height = fabsf(cam_corners[0][1] - cam_corners[1][1]);
  float cam_aspect = cam_width / cam_height;

  if (bgpic->flag & CAM_BGIMG_FLAG_CAMERA_CROP) {
    /* Crop. */
    if (image_aspect > cam_aspect) {
      scale[0][0] *= cam_height * image_aspect;
      scale[1][1] *= cam_height;
    }
    else {
      scale[0][0] *= cam_width;
      scale[1][1] *= cam_width / image_aspect;
    }
  }
  else if (bgpic->flag & CAM_BGIMG_FLAG_CAMERA_ASPECT) {
    /* Fit. */
    if (image_aspect > cam_aspect) {
      scale[0][0] *= cam_width;
      scale[1][1] *= cam_width / image_aspect;
    }
    else {
      scale[0][0] *= cam_height * image_aspect;
      scale[1][1] *= cam_height;
    }
  }
  else {
    /* Stretch. */
    scale[0][0] *= cam_width;
    scale[1][1] *= cam_height;
  }

  translate[3][0] = bgpic->offset[0];
  translate[3][1] = bgpic->offset[1];
  translate[3][2] = cam_corners[0][2];
  /* These lines are for keeping 2.80 behavior and could be removed to keep 2.79 behavior. */
  translate[3][0] *= min_ff(1.0f, cam_aspect);
  translate[3][1] /= max_ff(1.0f, cam_aspect) * (image_aspect / cam_aspect);
  /* quad is -1..1 so divide by 2. */
  scale[0][0] *= 0.5f * bgpic->scale * ((bgpic->flag & CAM_BGIMG_FLAG_FLIP_X) ? -1.0 : 1.0);
  scale[1][1] *= 0.5f * bgpic->scale * ((bgpic->flag & CAM_BGIMG_FLAG_FLIP_Y) ? -1.0 : 1.0);
  /* Camera shift. (middle of cam_corners) */
  translate[3][0] += (cam_corners[0][0] + cam_corners[2][0]) * 0.5f;
  translate[3][1] += (cam_corners[0][1] + cam_corners[2][1]) * 0.5f;

  mul_m4_series(rmat, translate, rotate, scale);
}

void OVERLAY_image_camera_cache_populate(OVERLAY_Data *vedata, Object *ob)
{
  OVERLAY_PrivateData *pd = vedata->stl->pd;
  OVERLAY_PassList *psl = vedata->psl;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  Camera *cam = ob->data;

  const bool show_frame = BKE_object_empty_image_frame_is_visible_in_view3d(ob, draw_ctx->rv3d);

  if (!show_frame || DRW_state_is_select()) {
    return;
  }

  float norm_obmat[4][4];
  normalize_m4_m4(norm_obmat, ob->obmat);

  for (CameraBGImage *bgpic = cam->bg_images.first; bgpic; bgpic = bgpic->next) {
    if (bgpic->flag & CAM_BGIMG_FLAG_DISABLED) {
      continue;
    }

    float aspect = 1.0;
    bool use_alpha_premult;
    float mat[4][4];

    /* retrieve the image we want to show, continue to next when no image could be found */
    GPUTexture *tex = image_camera_background_texture_get(
        bgpic, draw_ctx, pd, &aspect, &use_alpha_premult);

    if (tex) {
      image_camera_background_matrix_get(cam, bgpic, draw_ctx, aspect, mat);

      mul_m4_m4m4(mat, norm_obmat, mat);

      DRWPass *pass = (bgpic->flag & CAM_BGIMG_FLAG_FOREGROUND) ? psl->image_foreground_ps :
                                                                  psl->image_background_ps;
      GPUShader *sh = OVERLAY_shader_image();
      DRWShadingGroup *grp = DRW_shgroup_create(sh, pass);
      float color[4] = {1.0f, 1.0f, 1.0f, bgpic->alpha};
      DRW_shgroup_uniform_texture(grp, "imgTexture", tex);
      DRW_shgroup_uniform_bool_copy(grp, "imgPremultiplied", use_alpha_premult);
      DRW_shgroup_uniform_bool_copy(grp, "imgAlphaBlend", true);
      DRW_shgroup_uniform_bool_copy(grp, "imgLinear", !DRW_state_do_color_management());
      DRW_shgroup_uniform_bool_copy(grp, "depthSet", true);
      DRW_shgroup_uniform_vec4_copy(grp, "color", color);
      DRW_shgroup_call_obmat(grp, DRW_cache_empty_image_plane_get(), mat);
    }
  }
}

void OVERLAY_image_empty_cache_populate(OVERLAY_Data *vedata, Object *ob)
{
  OVERLAY_PassList *psl = vedata->psl;
  const DRWContextState *draw_ctx = DRW_context_state_get();
  const RegionView3D *rv3d = draw_ctx->rv3d;
  GPUTexture *tex = NULL;
  Image *ima = ob->data;
  float mat[4][4];

  const bool show_frame = BKE_object_empty_image_frame_is_visible_in_view3d(ob, rv3d);
  const bool show_image = show_frame && BKE_object_empty_image_data_is_visible_in_view3d(ob, rv3d);
  const bool use_alpha_blend = (ob->empty_image_flag & OB_EMPTY_IMAGE_USE_ALPHA_BLEND) != 0;
  const bool use_alpha_premult = ima && (ima->alpha_mode == IMA_ALPHA_PREMUL);

  if (!show_frame) {
    return;
  }

  {
    /* Calling 'BKE_image_get_size' may free the texture. Get the size from 'tex' instead,
     * see: T59347 */
    int size[2] = {0};
    if (ima != NULL) {
      tex = GPU_texture_from_blender(ima, ob->iuser, GL_TEXTURE_2D);
      if (tex) {
        size[0] = GPU_texture_orig_width(tex);
        size[1] = GPU_texture_orig_height(tex);
      }
    }
    CLAMP_MIN(size[0], 1);
    CLAMP_MIN(size[1], 1);

    float image_aspect[2];
    overlay_image_calc_aspect(ob->data, size, image_aspect);

    copy_m4_m4(mat, ob->obmat);
    mul_v3_fl(mat[0], image_aspect[0] * 0.5f * ob->empty_drawsize);
    mul_v3_fl(mat[1], image_aspect[1] * 0.5f * ob->empty_drawsize);
    madd_v3_v3fl(mat[3], mat[0], ob->ima_ofs[0] * 2.0f + 1.0f);
    madd_v3_v3fl(mat[3], mat[1], ob->ima_ofs[1] * 2.0f + 1.0f);
  }

  /* Use the actual depth if we are doing depth tests to determine the distance to the object */
  char depth_mode = DRW_state_is_depth() ? OB_EMPTY_IMAGE_DEPTH_DEFAULT : ob->empty_image_depth;
  DRWPass *pass = NULL;
  switch (depth_mode) {
    case OB_EMPTY_IMAGE_DEPTH_DEFAULT:
      pass = (use_alpha_blend) ? psl->image_empties_blend_ps : psl->image_empties_ps;
      break;
    case OB_EMPTY_IMAGE_DEPTH_BACK:
      pass = psl->image_empties_back_ps;
      break;
    case OB_EMPTY_IMAGE_DEPTH_FRONT:
      pass = psl->image_empties_front_ps;
      break;
  }

  if (show_frame) {
    OVERLAY_ExtraCallBuffers *cb = OVERLAY_extra_call_buffer_get(vedata, ob);
    float *color;
    DRW_object_wire_theme_get(ob, draw_ctx->view_layer, &color);
    OVERLAY_empty_shape(cb, mat, 1.0f, OB_EMPTY_IMAGE, color);
  }

  if (show_image && tex && ((ob->color[3] > 0.0f) || !use_alpha_blend)) {
    GPUShader *sh = OVERLAY_shader_image();
    DRWShadingGroup *grp = DRW_shgroup_create(sh, pass);
    DRW_shgroup_uniform_texture(grp, "imgTexture", tex);
    DRW_shgroup_uniform_bool_copy(grp, "imgPremultiplied", use_alpha_premult);
    DRW_shgroup_uniform_bool_copy(grp, "imgAlphaBlend", use_alpha_blend);
    DRW_shgroup_uniform_bool_copy(grp, "imgLinear", false);
    DRW_shgroup_uniform_bool_copy(grp, "depthSet", depth_mode != OB_EMPTY_IMAGE_DEPTH_DEFAULT);
    DRW_shgroup_uniform_vec4_copy(grp, "color", ob->color);
    DRW_shgroup_call_obmat(grp, DRW_cache_empty_image_plane_get(), mat);
  }
}

void OVERLAY_image_cache_finish(OVERLAY_Data *UNUSED(vedata))
{
  /* Order by Z depth. */
}

void OVERLAY_image_draw(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;
  /* TODO better ordering with other passes. */
  DRW_draw_pass(psl->image_background_ps);
  DRW_draw_pass(psl->image_empties_back_ps);

  DRW_draw_pass(psl->image_empties_ps);
  DRW_draw_pass(psl->image_empties_blend_ps);

  DRW_draw_pass(psl->image_empties_front_ps);
  DRW_draw_pass(psl->image_foreground_ps);

  OVERLAY_image_free_movieclips_textures(vedata);
}
