// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cfloat>

#include "Common/Data/Convert/ColorConv.h"
#include "Common/Profiler/Profiler.h"
#include "Common/LogReporting.h"
#include "Common/Math/lin/matrix4x4.h"
#include "Core/Config.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/SplineCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"

#define QUAD_INDICES_MAX 65536

enum {
	TRANSFORMED_VERTEX_BUFFER_SIZE = VERTEX_BUFFER_MAX * sizeof(TransformedVertex)
};

DrawEngineCommon::DrawEngineCommon() : decoderMap_(16) {
	if (g_Config.bVertexDecoderJit && (g_Config.iCpuCore == (int)CPUCore::JIT || g_Config.iCpuCore == (int)CPUCore::JIT_IR)) {
		decJitCache_ = new VertexDecoderJitCache();
	}
	transformed_ = (TransformedVertex *)AllocateMemoryPages(TRANSFORMED_VERTEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
	transformedExpanded_ = (TransformedVertex *)AllocateMemoryPages(3 * TRANSFORMED_VERTEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
	decoded_ = (u8 *)AllocateMemoryPages(DECODED_VERTEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
	decIndex_ = (u16 *)AllocateMemoryPages(DECODED_INDEX_BUFFER_SIZE, MEM_PROT_READ | MEM_PROT_WRITE);
}

DrawEngineCommon::~DrawEngineCommon() {
	FreeMemoryPages(decoded_, DECODED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(decIndex_, DECODED_INDEX_BUFFER_SIZE);
	FreeMemoryPages(transformed_, TRANSFORMED_VERTEX_BUFFER_SIZE);
	FreeMemoryPages(transformedExpanded_, 3 * TRANSFORMED_VERTEX_BUFFER_SIZE);
	delete decJitCache_;
	decoderMap_.Iterate([&](const uint32_t vtype, VertexDecoder *decoder) {
		delete decoder;
	});
	ClearSplineBezierWeights();
}

void DrawEngineCommon::Init() {
	NotifyConfigChanged();
}

VertexDecoder *DrawEngineCommon::GetVertexDecoder(u32 vtype) {
	VertexDecoder *dec;
	if (decoderMap_.Get(vtype, &dec))
		return dec;
	dec = new VertexDecoder();
	_assert_(dec);
	dec->SetVertexType(vtype, decOptions_, decJitCache_);
	decoderMap_.Insert(vtype, dec);
	return dec;
}

std::vector<std::string> DrawEngineCommon::DebugGetVertexLoaderIDs() {
	std::vector<std::string> ids;
	decoderMap_.Iterate([&](const uint32_t vtype, VertexDecoder *decoder) {
		std::string id;
		id.resize(sizeof(vtype));
		memcpy(&id[0], &vtype, sizeof(vtype));
		ids.push_back(id);
	});
	return ids;
}

std::string DrawEngineCommon::DebugGetVertexLoaderString(std::string id, DebugShaderStringType stringType) {
	u32 mapId;
	memcpy(&mapId, &id[0], sizeof(mapId));
	VertexDecoder *dec;
	if (decoderMap_.Get(mapId, &dec)) {
		return dec->GetString(stringType);
	} else {
		return "N/A";
	}
}

static Vec3f ClipToScreen(const Vec4f& coords) {
	float xScale = gstate.getViewportXScale();
	float xCenter = gstate.getViewportXCenter();
	float yScale = gstate.getViewportYScale();
	float yCenter = gstate.getViewportYCenter();
	float zScale = gstate.getViewportZScale();
	float zCenter = gstate.getViewportZCenter();

	float x = coords.x * xScale / coords.w + xCenter;
	float y = coords.y * yScale / coords.w + yCenter;
	float z = coords.z * zScale / coords.w + zCenter;

	// 16 = 0xFFFF / 4095.9375
	return Vec3f(x * 16 - gstate.getOffsetX16(), y * 16 - gstate.getOffsetY16(), z);
}

static Vec3f ScreenToDrawing(const Vec3f& coords) {
	Vec3f ret;
	ret.x = coords.x * (1.0f / 16.0f);
	ret.y = coords.y * (1.0f / 16.0f);
	ret.z = coords.z;
	return ret;
}

void DrawEngineCommon::NotifyConfigChanged() {
	if (decJitCache_)
		decJitCache_->Clear();
	lastVType_ = -1;
	dec_ = nullptr;
	decoderMap_.Iterate([&](const uint32_t vtype, VertexDecoder *decoder) {
		delete decoder;
	});
	decoderMap_.Clear();
	ClearTrackedVertexArrays();

	useHWTransform_ = g_Config.bHardwareTransform;
	useHWTessellation_ = UpdateUseHWTessellation(g_Config.bHardwareTessellation);
	decOptions_.applySkinInDecode = g_Config.bSoftwareSkinning;
}

u32 DrawEngineCommon::NormalizeVertices(u8 *outPtr, u8 *bufPtr, const u8 *inPtr, int lowerBound, int upperBound, u32 vertType, int *vertexSize) {
	const u32 vertTypeID = GetVertTypeID(vertType, gstate.getUVGenMode(), decOptions_.applySkinInDecode);
	VertexDecoder *dec = GetVertexDecoder(vertTypeID);
	if (vertexSize)
		*vertexSize = dec->VertexSize();
	return DrawEngineCommon::NormalizeVertices(outPtr, bufPtr, inPtr, dec, lowerBound, upperBound, vertType);
}

void DrawEngineCommon::DispatchSubmitImm(GEPrimitiveType prim, TransformedVertex *buffer, int vertexCount, int cullMode, bool continuation) {
	// Instead of plumbing through properly (we'd need to inject these pretransformed vertices in the middle
	// of SoftwareTransform(), which would take a lot of refactoring), we'll cheat and just turn these into
	// through vertices.
	// Since the only known use is Thrillville and it only uses it to clear, we just use color and pos.
	struct ImmVertex {
		float uv[2];
		uint32_t color;
		float xyz[3];
	};
	std::vector<ImmVertex> temp;
	temp.resize(vertexCount);
	uint32_t color1Used = 0;
	for (int i = 0; i < vertexCount; i++) {
		// Since we're sending through, scale back up to w/h.
		temp[i].uv[0] = buffer[i].u * gstate.getTextureWidth(0);
		temp[i].uv[1] = buffer[i].v * gstate.getTextureHeight(0);
		temp[i].color = buffer[i].color0_32;
		temp[i].xyz[0] = buffer[i].pos[0];
		temp[i].xyz[1] = buffer[i].pos[1];
		temp[i].xyz[2] = buffer[i].pos[2];
		color1Used |= buffer[i].color1_32;
	}
	int vtype = GE_VTYPE_TC_FLOAT | GE_VTYPE_POS_FLOAT | GE_VTYPE_COL_8888 | GE_VTYPE_THROUGH;
	// TODO: Handle fog and secondary color somehow?

	if (gstate.isFogEnabled() && !gstate.isModeThrough()) {
		WARN_LOG_REPORT_ONCE(geimmfog, G3D, "Imm vertex used fog");
	}
	if (color1Used != 0 && gstate.isUsingSecondaryColor() && !gstate.isModeThrough()) {
		WARN_LOG_REPORT_ONCE(geimmcolor1, G3D, "Imm vertex used secondary color");
	}

	bool prevThrough = gstate.isModeThrough();
	// Code checks this reg directly, not just the vtype ID.
	if (!prevThrough) {
		gstate.vertType |= GE_VTYPE_THROUGH;
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE);
	}

	int bytesRead;
	uint32_t vertTypeID = GetVertTypeID(vtype, 0, decOptions_.applySkinInDecode);

	bool clockwise = !gstate.isCullEnabled() || gstate.getCullMode() == cullMode;
	SubmitPrim(&temp[0], nullptr, prim, vertexCount, vertTypeID, clockwise, &bytesRead);
	DispatchFlush();

	if (!prevThrough) {
		gstate.vertType &= ~GE_VTYPE_THROUGH;
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_CULLRANGE);
	}
}

// Gated by DIRTY_CULL_PLANES
void DrawEngineCommon::UpdatePlanes() {
	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	// TODO: Create a Matrix4x3ByMatrix4x3, and Matrix4x4ByMatrix4x3?
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);

	// Next, we need to apply viewport, scissor, region, and even offset - but only for X/Y.
	// Note that the PSP does not clip against the viewport.
	const Vec2f baseOffset = Vec2f(gstate.getOffsetX(), gstate.getOffsetY());
	// Region1 (rate) is used as an X1/Y1 here, matching PSP behavior.
	minOffset_ = baseOffset + Vec2f(std::max(gstate.getRegionRateX() - 0x100, gstate.getScissorX1()), std::max(gstate.getRegionRateY() - 0x100, gstate.getScissorY1())) - Vec2f(1.0f, 1.0f);
	maxOffset_ = baseOffset + Vec2f(std::min(gstate.getRegionX2(), gstate.getScissorX2()), std::min(gstate.getRegionY2(), gstate.getScissorY2())) + Vec2f(1.0f, 1.0f);

	// Now let's apply the viewport to our scissor/region + offset range.
	Vec2f inverseViewportScale = Vec2f(1.0f / gstate.getViewportXScale(), 1.0f / gstate.getViewportYScale());
	Vec2f minViewport = (minOffset_ - Vec2f(gstate.getViewportXCenter(), gstate.getViewportYCenter())) * inverseViewportScale;
	Vec2f maxViewport = (maxOffset_ - Vec2f(gstate.getViewportXCenter(), gstate.getViewportYCenter())) * inverseViewportScale;

	Vec2f viewportInvSize = Vec2f(1.0f / (maxViewport.x - minViewport.x), 1.0f / (maxViewport.y - minViewport.y));

	Lin::Matrix4x4 applyViewport{};
	// Scale to the viewport's size.
	applyViewport.xx = 2.0f * viewportInvSize.x;
	applyViewport.yy = 2.0f * viewportInvSize.y;
	applyViewport.zz = 1.0f;
	applyViewport.ww = 1.0f;
	// And offset to the viewport's centers.
	applyViewport.wx = -(maxViewport.x + minViewport.x) * viewportInvSize.x;
	applyViewport.wy = -(maxViewport.y + minViewport.y) * viewportInvSize.y;

	float mtx[16];
	Matrix4ByMatrix4(mtx, worldviewproj, applyViewport.m);

	planes_[0].Set(mtx[3] - mtx[0], mtx[7] - mtx[4], mtx[11] - mtx[8], mtx[15] - mtx[12]);  // Right
	planes_[1].Set(mtx[3] + mtx[0], mtx[7] + mtx[4], mtx[11] + mtx[8], mtx[15] + mtx[12]);  // Left
	planes_[2].Set(mtx[3] + mtx[1], mtx[7] + mtx[5], mtx[11] + mtx[9], mtx[15] + mtx[13]);  // Bottom
	planes_[3].Set(mtx[3] - mtx[1], mtx[7] - mtx[5], mtx[11] - mtx[9], mtx[15] - mtx[13]);  // Top
	planes_[4].Set(mtx[3] + mtx[2], mtx[7] + mtx[6], mtx[11] + mtx[10], mtx[15] + mtx[14]); // Near
	planes_[5].Set(mtx[3] - mtx[2], mtx[7] - mtx[6], mtx[11] - mtx[10], mtx[15] - mtx[14]); // Far
}

// This code has plenty of potential for optimization.
//
// It does the simplest and safest test possible: If all points of a bbox is outside a single of
// our clipping planes, we reject the box. Tighter bounds would be desirable but would take more calculations.
// The name is a slight misnomer, because any bounding shape will work, not just boxes.
//
// Potential optimizations:
// * SIMD-ify the plane culling, and also the vertex data conversion (could even group together xxxxyyyyzzzz for example)
// * Compute min/max of the verts, and then compute a bounding sphere and check that against the planes.
//   - Less accurate, but..
//   - Only requires six plane evaluations then.

bool DrawEngineCommon::TestBoundingBox(const void *vdata, const void *inds, int vertexCount, u32 vertType) {
	// Grab temp buffer space from large offsets in decoded_. Not exactly safe for large draws.
	if (vertexCount > 1024) {
		return true;
	}

	SimpleVertex *corners = (SimpleVertex *)(decoded_ + 65536 * 12);
	float *verts = (float *)(decoded_ + 65536 * 18);
	int vertStride = 3;

	// Although this may lead to drawing that shouldn't happen, the viewport is more complex on VR.
	// Let's always say objects are within bounds.
	if (gstate_c.Use(GPU_USE_VIRTUAL_REALITY))
		return true;

	// Due to world matrix updates per "thing", this isn't quite as effective as it could be if we did world transform
	// in here as well. Though, it still does cut down on a lot of updates in Tekken 6.
	if (gstate_c.IsDirty(DIRTY_CULL_PLANES)) {
		UpdatePlanes();
		gpuStats.numPlaneUpdates++;
		gstate_c.Clean(DIRTY_CULL_PLANES);
	}

	// Try to skip NormalizeVertices if it's pure positions. No need to bother with a vertex decoder
	// and a large vertex format.
	if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_FLOAT && !inds) {
		verts = (float *)vdata;
	} else if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_8BIT && !inds) {
		const s8 *vtx = (const s8 *)vdata;
		for (int i = 0; i < vertexCount * 3; i++) {
			verts[i] = vtx[i] * (1.0f / 128.0f);
		}
	} else if ((vertType & 0xFFFFFF) == GE_VTYPE_POS_16BIT && !inds) {
		const s16 *vtx = (const s16 *)vdata;
		for (int i = 0; i < vertexCount * 3; i++) {
			verts[i] = vtx[i] * (1.0f / 32768.0f);
		}
	} else {
		// Simplify away indices, bones, and morph before proceeding.
		u8 *temp_buffer = decoded_ + 65536 * 24;

		if ((inds || (vertType & (GE_VTYPE_WEIGHT_MASK | GE_VTYPE_MORPHCOUNT_MASK)))) {
			u16 indexLowerBound = 0;
			u16 indexUpperBound = (u16)vertexCount - 1;

			if (vertexCount > 0 && inds) {
				GetIndexBounds(inds, vertexCount, vertType, &indexLowerBound, &indexUpperBound);
			}
			// TODO: Avoid normalization if just plain skinning.
			// Force software skinning.
			bool wasApplyingSkinInDecode = decOptions_.applySkinInDecode;
			decOptions_.applySkinInDecode = true;
			NormalizeVertices((u8 *)corners, temp_buffer, (const u8 *)vdata, indexLowerBound, indexUpperBound, vertType);
			decOptions_.applySkinInDecode = wasApplyingSkinInDecode;

			IndexConverter conv(vertType, inds);
			for (int i = 0; i < vertexCount; i++) {
				verts[i * 3] = corners[conv(i)].pos.x;
				verts[i * 3 + 1] = corners[conv(i)].pos.y;
				verts[i * 3 + 2] = corners[conv(i)].pos.z;
			}
		} else {
			// Simple, most common case.
			VertexDecoder *dec = GetVertexDecoder(vertType);
			int stride = dec->VertexSize();
			int offset = dec->posoff;
			switch (vertType & GE_VTYPE_POS_MASK) {
			case GE_VTYPE_POS_8BIT:
				for (int i = 0; i < vertexCount; i++) {
					const s8 *data = (const s8 *)vdata + i * stride + offset;
					for (int j = 0; j < 3; j++) {
						verts[i * 3 + j] = data[j] * (1.0f / 128.0f);
					}
				}
				break;
			case GE_VTYPE_POS_16BIT:
				for (int i = 0; i < vertexCount; i++) {
					const s16 *data = ((const s16 *)((const s8 *)vdata + i * stride + offset));
					for (int j = 0; j < 3; j++) {
						verts[i * 3 + j] = data[j] * (1.0f / 32768.0f);
					}
				}
				break;
			case GE_VTYPE_POS_FLOAT:
				// No need to copy in this case, we can just read directly from the source format with a stride.
				verts = (float *)((uint8_t *)vdata + offset);
				vertStride = stride / 4;
				// Previous code:
				// for (int i = 0; i < vertexCount; i++)
				//   memcpy(&verts[i * 3], (const u8 *)vdata + stride * i + offset, sizeof(float) * 3);
				break;
			}
		}
	}

	// Note: near/far are not checked without clamp/clip enabled, so we skip those planes.
	int totalPlanes = gstate.isDepthClampEnabled() ? 6 : 4;
	for (int plane = 0; plane < totalPlanes; plane++) {
		int inside = 0;
		int out = 0;
		for (int i = 0; i < vertexCount; i++) {
			// Test against the frustum planes, and count.
			// TODO: We should test 4 vertices at a time using SIMD.
			// I guess could also test one vertex against 4 planes at a time, though a lot of waste at the common case of 6.
			const float *pos = verts + i * vertStride;
			float value = planes_[plane].Test(pos);
			if (value <= -FLT_EPSILON)  // Not sure why we use exactly this value. Probably '< 0' would do.
				out++;
			else
				inside++;
		}

		// No vertices inside this one plane? Don't need to draw.
		if (inside == 0) {
			// All out - but check for X and Y if the offset was near the cullbox edge.
			bool outsideEdge = false;
			switch (plane) {
			case 0: outsideEdge = maxOffset_.x >= 4096.0f; break;
			case 1: outsideEdge = minOffset_.x < 1.0f; break;
			case 2: outsideEdge = minOffset_.y < 1.0f; break;
			case 3: outsideEdge = maxOffset_.y >= 4096.0f; break;
			}

			// Only consider this outside if offset + scissor/region is fully inside the cullbox.
			if (!outsideEdge)
				return false;
		}

		// Any out. For testing that the planes are in the right locations.
		// if (out != 0) return false;
	}
	return true;
}

// TODO: This probably is not the best interface.
bool DrawEngineCommon::GetCurrentSimpleVertices(int count, std::vector<GPUDebugVertex> &vertices, std::vector<u16> &indices) {
	// This is always for the current vertices.
	u16 indexLowerBound = 0;
	u16 indexUpperBound = count - 1;

	if (!Memory::IsValidAddress(gstate_c.vertexAddr) || count == 0)
		return false;

	bool savedVertexFullAlpha = gstate_c.vertexFullAlpha;

	if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		const u8 *inds = Memory::GetPointer(gstate_c.indexAddr);
		const u16_le *inds16 = (const u16_le *)inds;
		const u32_le *inds32 = (const u32_le *)inds;

		if (inds) {
			GetIndexBounds(inds, count, gstate.vertType, &indexLowerBound, &indexUpperBound);
			indices.resize(count);
			switch (gstate.vertType & GE_VTYPE_IDX_MASK) {
			case GE_VTYPE_IDX_8BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds[i];
				}
				break;
			case GE_VTYPE_IDX_16BIT:
				for (int i = 0; i < count; ++i) {
					indices[i] = inds16[i];
				}
				break;
			case GE_VTYPE_IDX_32BIT:
				WARN_LOG_REPORT_ONCE(simpleIndexes32, G3D, "SimpleVertices: Decoding 32-bit indexes");
				for (int i = 0; i < count; ++i) {
					// These aren't documented and should be rare.  Let's bounds check each one.
					if (inds32[i] != (u16)inds32[i]) {
						ERROR_LOG_REPORT_ONCE(simpleIndexes32Bounds, G3D, "SimpleVertices: Index outside 16-bit range");
					}
					indices[i] = (u16)inds32[i];
				}
				break;
			}
		} else {
			indices.clear();
		}
	} else {
		indices.clear();
	}

	static std::vector<u32> temp_buffer;
	static std::vector<SimpleVertex> simpleVertices;
	temp_buffer.resize(std::max((int)indexUpperBound, 8192) * 128 / sizeof(u32));
	simpleVertices.resize(indexUpperBound + 1);
	NormalizeVertices((u8 *)(&simpleVertices[0]), (u8 *)(&temp_buffer[0]), Memory::GetPointerUnchecked(gstate_c.vertexAddr), indexLowerBound, indexUpperBound, gstate.vertType);

	float world[16];
	float view[16];
	float worldview[16];
	float worldviewproj[16];
	ConvertMatrix4x3To4x4(world, gstate.worldMatrix);
	ConvertMatrix4x3To4x4(view, gstate.viewMatrix);
	Matrix4ByMatrix4(worldview, world, view);
	Matrix4ByMatrix4(worldviewproj, worldview, gstate.projMatrix);

	vertices.resize(indexUpperBound + 1);
	uint32_t vertType = gstate.vertType;
	for (int i = indexLowerBound; i <= indexUpperBound; ++i) {
		const SimpleVertex &vert = simpleVertices[i];

		if ((vertType & GE_VTYPE_THROUGH) != 0) {
			if (vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0];
				vertices[i].v = vert.uv[1];
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			vertices[i].x = vert.pos.x;
			vertices[i].y = vert.pos.y;
			vertices[i].z = vert.pos.z;
			if (vertType & GE_VTYPE_COL_MASK) {
				memcpy(vertices[i].c, vert.color, sizeof(vertices[i].c));
			} else {
				memset(vertices[i].c, 0, sizeof(vertices[i].c));
			}
			vertices[i].nx = 0;  // No meaningful normals in through mode
			vertices[i].ny = 0;
			vertices[i].nz = 1.0f;
		} else {
			float clipPos[4];
			Vec3ByMatrix44(clipPos, vert.pos.AsArray(), worldviewproj);
			Vec3f screenPos = ClipToScreen(clipPos);
			Vec3f drawPos = ScreenToDrawing(screenPos);

			if (vertType & GE_VTYPE_TC_MASK) {
				vertices[i].u = vert.uv[0] * (float)gstate.getTextureWidth(0);
				vertices[i].v = vert.uv[1] * (float)gstate.getTextureHeight(0);
			} else {
				vertices[i].u = 0.0f;
				vertices[i].v = 0.0f;
			}
			// Should really have separate coordinates for before and after transform.
			vertices[i].x = drawPos.x;
			vertices[i].y = drawPos.y;
			vertices[i].z = drawPos.z;
			if (vertType & GE_VTYPE_COL_MASK) {
				memcpy(vertices[i].c, vert.color, sizeof(vertices[i].c));
			} else {
				memset(vertices[i].c, 0, sizeof(vertices[i].c));
			}
			vertices[i].nx = vert.nrm.x;
			vertices[i].ny = vert.nrm.y;
			vertices[i].nz = vert.nrm.z;
		}
	}

	gstate_c.vertexFullAlpha = savedVertexFullAlpha;

	return true;
}

// This normalizes a set of vertices in any format to SimpleVertex format, by processing away morphing AND skinning.
// The rest of the transform pipeline like lighting will go as normal, either hardware or software.
// The implementation is initially a bit inefficient but shouldn't be a big deal.
// An intermediate buffer of not-easy-to-predict size is stored at bufPtr.
u32 DrawEngineCommon::NormalizeVertices(u8 *outPtr, u8 *bufPtr, const u8 *inPtr, VertexDecoder *dec, int lowerBound, int upperBound, u32 vertType) {
	// First, decode the vertices into a GPU compatible format. This step can be eliminated but will need a separate
	// implementation of the vertex decoder.
	dec->DecodeVerts(bufPtr, inPtr, &gstate_c.uv, lowerBound, upperBound);

	// OK, morphing eliminated but bones still remain to be taken care of.
	// Let's do a partial software transform where we only do skinning.

	VertexReader reader(bufPtr, dec->GetDecVtxFmt(), vertType);

	SimpleVertex *sverts = (SimpleVertex *)outPtr;

	const u8 defaultColor[4] = {
		(u8)gstate.getMaterialAmbientR(),
		(u8)gstate.getMaterialAmbientG(),
		(u8)gstate.getMaterialAmbientB(),
		(u8)gstate.getMaterialAmbientA(),
	};

	// Let's have two separate loops, one for non skinning and one for skinning.
	if (!dec->skinInDecode && (vertType & GE_VTYPE_WEIGHT_MASK) != GE_VTYPE_WEIGHT_NONE) {
		int numBoneWeights = vertTypeGetNumBoneWeights(vertType);
		for (int i = lowerBound; i <= upperBound; i++) {
			reader.Goto(i - lowerBound);
			SimpleVertex &sv = sverts[i];
			if (vertType & GE_VTYPE_TC_MASK) {
				reader.ReadUV(sv.uv);
			}

			if (vertType & GE_VTYPE_COL_MASK) {
				sv.color_32 = reader.ReadColor0_8888();
			} else {
				memcpy(sv.color, defaultColor, 4);
			}

			float nrm[3], pos[3];
			float bnrm[3], bpos[3];

			if (vertType & GE_VTYPE_NRM_MASK) {
				// Normals are generated during tessellation anyway, not sure if any need to supply
				reader.ReadNrm(nrm);
			} else {
				nrm[0] = 0;
				nrm[1] = 0;
				nrm[2] = 1.0f;
			}
			reader.ReadPos(pos);

			// Apply skinning transform directly
			float weights[8];
			reader.ReadWeights(weights);
			// Skinning
			Vec3Packedf psum(0, 0, 0);
			Vec3Packedf nsum(0, 0, 0);
			for (int w = 0; w < numBoneWeights; w++) {
				if (weights[w] != 0.0f) {
					Vec3ByMatrix43(bpos, pos, gstate.boneMatrix + w * 12);
					Vec3Packedf tpos(bpos);
					psum += tpos * weights[w];

					Norm3ByMatrix43(bnrm, nrm, gstate.boneMatrix + w * 12);
					Vec3Packedf tnorm(bnrm);
					nsum += tnorm * weights[w];
				}
			}
			sv.pos = psum;
			sv.nrm = nsum;
		}
	} else {
		for (int i = lowerBound; i <= upperBound; i++) {
			reader.Goto(i - lowerBound);
			SimpleVertex &sv = sverts[i];
			if (vertType & GE_VTYPE_TC_MASK) {
				reader.ReadUV(sv.uv);
			} else {
				sv.uv[0] = 0.0f;  // This will get filled in during tessellation
				sv.uv[1] = 0.0f;
			}
			if (vertType & GE_VTYPE_COL_MASK) {
				sv.color_32 = reader.ReadColor0_8888();
			} else {
				memcpy(sv.color, defaultColor, 4);
			}
			if (vertType & GE_VTYPE_NRM_MASK) {
				// Normals are generated during tessellation anyway, not sure if any need to supply
				reader.ReadNrm((float *)&sv.nrm);
			} else {
				sv.nrm.x = 0.0f;
				sv.nrm.y = 0.0f;
				sv.nrm.z = 1.0f;
			}
			reader.ReadPos((float *)&sv.pos);
		}
	}

	// Okay, there we are! Return the new type (but keep the index bits)
	return GE_VTYPE_TC_FLOAT | GE_VTYPE_COL_8888 | GE_VTYPE_NRM_FLOAT | GE_VTYPE_POS_FLOAT | (vertType & (GE_VTYPE_IDX_MASK | GE_VTYPE_THROUGH));
}

void DrawEngineCommon::ApplyFramebufferRead(FBOTexState *fboTexState) {
	if (gstate_c.Use(GPU_USE_FRAMEBUFFER_FETCH)) {
		*fboTexState = FBO_TEX_READ_FRAMEBUFFER;
	} else {
		gpuStats.numCopiesForShaderBlend++;
		*fboTexState = FBO_TEX_COPY_BIND_TEX;
	}

	gstate_c.Dirty(DIRTY_SHADERBLEND);
}

int DrawEngineCommon::ComputeNumVertsToDecode() const {
	int sum = 0;
	for (int i = 0; i < numDrawVerts_; i++) {
		sum += drawVerts_[i].indexUpperBound + 1 - drawVerts_[i].indexLowerBound;
	}
	return sum;
}

int DrawEngineCommon::ExtendNonIndexedPrim(const uint32_t *cmd, const uint32_t *stall, u32 vertTypeID, bool clockwise, int *bytesRead, bool isTriangle) {
	const uint32_t *start = cmd;
	int prevDrawVerts = numDrawVerts_ - 1;
	DeferredVerts &dv = drawVerts_[prevDrawVerts];
	int offset = dv.vertexCount;

	_dbg_assert_(numDrawInds_ <= MAX_DEFERRED_DRAW_INDS);  // if it's equal, the check below will take care of it before any action is taken.
	_dbg_assert_(numDrawVerts_ > 0);

	while (cmd != stall) {
		uint32_t data = *cmd;
		if ((data & 0xFFF80000) != 0x04000000) {
			break;
		}
		GEPrimitiveType newPrim = static_cast<GEPrimitiveType>((data >> 16) & 7);
		if (IsTrianglePrim(newPrim) != isTriangle)
			break;
		int vertexCount = data & 0xFFFF;
		if (numDrawInds_ >= MAX_DEFERRED_DRAW_INDS || vertexCountInDrawCalls_ + offset + vertexCount > VERTEX_BUFFER_MAX) {
			break;
		}
		DeferredInds &di = drawInds_[numDrawInds_++];
		di.indexType = 0;
		di.prim = newPrim;
		di.clockwise = clockwise;
		di.vertexCount = vertexCount;
		di.vertDecodeIndex = prevDrawVerts;
		di.offset = offset;
		offset += vertexCount;
		cmd++;
	}

	int totalCount = offset - dv.vertexCount;
	dv.vertexCount = offset;
	dv.indexUpperBound = dv.vertexCount - 1;
	vertexCountInDrawCalls_ += totalCount;
	*bytesRead = totalCount * dec_->VertexSize();
	return cmd - start;
}

void DrawEngineCommon::SkipPrim(GEPrimitiveType prim, int vertexCount, u32 vertTypeID, int *bytesRead) {
	if (!indexGen.PrimCompatible(prevPrim_, prim)) {
		DispatchFlush();
	}

	// This isn't exactly right, if we flushed, since prims can straddle previous calls.
	// But it generally works for common usage.
	if (prim == GE_PRIM_KEEP_PREVIOUS) {
		// Has to be set to something, let's assume POINTS (0) if no previous.
		if (prevPrim_ == GE_PRIM_INVALID)
			prevPrim_ = GE_PRIM_POINTS;
		prim = prevPrim_;
	} else {
		prevPrim_ = prim;
	}

	// If vtype has changed, setup the vertex decoder.
	if (vertTypeID != lastVType_ || !dec_) {
		dec_ = GetVertexDecoder(vertTypeID);
		lastVType_ = vertTypeID;
	}

	*bytesRead = vertexCount * dec_->VertexSize();
}

// vertTypeID is the vertex type but with the UVGen mode smashed into the top bits.
bool DrawEngineCommon::SubmitPrim(const void *verts, const void *inds, GEPrimitiveType prim, int vertexCount, u32 vertTypeID, bool clockwise, int *bytesRead) {
	if (!indexGen.PrimCompatible(prevPrim_, prim) || numDrawVerts_ >= MAX_DEFERRED_DRAW_VERTS || numDrawInds_ >= MAX_DEFERRED_DRAW_INDS || vertexCountInDrawCalls_ + vertexCount > VERTEX_BUFFER_MAX) {
		DispatchFlush();
	}
	_dbg_assert_(numDrawVerts_ < MAX_DEFERRED_DRAW_VERTS);
	_dbg_assert_(numDrawInds_ < MAX_DEFERRED_DRAW_INDS);

	// This isn't exactly right, if we flushed, since prims can straddle previous calls.
	// But it generally works for common usage.
	if (prim == GE_PRIM_KEEP_PREVIOUS) {
		// Has to be set to something, let's assume POINTS (0) if no previous.
		if (prevPrim_ == GE_PRIM_INVALID)
			prevPrim_ = GE_PRIM_POINTS;
		prim = prevPrim_;
	} else {
		prevPrim_ = prim;
	}

	// If vtype has changed, setup the vertex decoder. Don't need to nullcheck dec_ since we set lastVType_ to an invalid value whenever we null it.
	if (vertTypeID != lastVType_) {
		dec_ = GetVertexDecoder(vertTypeID);
		lastVType_ = vertTypeID;
	}

	*bytesRead = vertexCount * dec_->VertexSize();

	// Check that we have enough vertices to form the requested primitive.
	if (vertexCount < 3 && ((vertexCount < 2 && prim > 0) || (prim > GE_PRIM_LINE_STRIP && prim != GE_PRIM_RECTANGLES)))
		return false;

	bool applySkin = (vertTypeID & GE_VTYPE_WEIGHT_MASK) && decOptions_.applySkinInDecode;

	DeferredInds &di = drawInds_[numDrawInds_++];
	di.inds = inds;
	di.indexType = (vertTypeID & GE_VTYPE_IDX_MASK) >> GE_VTYPE_IDX_SHIFT;
	di.prim = prim;
	di.clockwise = clockwise;
	di.vertexCount = vertexCount;
	di.vertDecodeIndex = numDrawVerts_;
	di.offset = 0;

	_dbg_assert_(numDrawVerts_ <= MAX_DEFERRED_DRAW_VERTS);
	_dbg_assert_(numDrawInds_ <= MAX_DEFERRED_DRAW_INDS);

	if (inds && numDrawVerts_ > decodeVertsCounter_ && drawVerts_[numDrawVerts_ - 1].verts == verts && !applySkin) {
		// Same vertex pointer as a previous un-decoded draw call - let's just extend the decode!
		di.vertDecodeIndex = numDrawVerts_ - 1;
		DeferredVerts &dv = drawVerts_[numDrawVerts_ - 1];
		u16 lb;
		u16 ub;
		GetIndexBounds(inds, vertexCount, vertTypeID, &lb, &ub);
		if (lb < dv.indexLowerBound)
			dv.indexLowerBound = lb;
		if (ub > dv.indexUpperBound)
			dv.indexUpperBound = ub;
	} else {
		// Record a new draw, and a new index gen.
		DeferredVerts &dv = drawVerts_[numDrawVerts_++];
		dv.verts = verts;
		dv.vertexCount = vertexCount;
		dv.uvScale = gstate_c.uv;
		if (inds) {
			GetIndexBounds(inds, vertexCount, vertTypeID, &dv.indexLowerBound, &dv.indexUpperBound);
		} else {
			dv.indexLowerBound = 0;
			dv.indexUpperBound = vertexCount - 1;
		}
	}

	vertexCountInDrawCalls_ += vertexCount;

	if (prim == GE_PRIM_RECTANGLES && (gstate.getTextureAddress(0) & 0x3FFFFFFF) == (gstate.getFrameBufAddress() & 0x3FFFFFFF)) {
		// This prevents issues with consecutive self-renders in Ridge Racer.
		gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
		DispatchFlush();
	}
	return true;
}

void DrawEngineCommon::DecodeVerts(u8 *dest) {
	int i = decodeVertsCounter_;
	int stride = (int)dec_->GetDecVtxFmt().stride;
	for (; i < numDrawVerts_; i++) {
		DeferredVerts &dv = drawVerts_[i];

		int indexLowerBound = dv.indexLowerBound;
		drawVertexOffsets_[i] = decodedVerts_ - indexLowerBound;

		int indexUpperBound = dv.indexUpperBound;
		// Decode the verts (and at the same time apply morphing/skinning). Simple.
		dec_->DecodeVerts(dest + decodedVerts_ * stride, dv.verts, &dv.uvScale, indexLowerBound, indexUpperBound);
		decodedVerts_ += indexUpperBound - indexLowerBound + 1;
	}
	decodeVertsCounter_ = i;
}

void DrawEngineCommon::DecodeInds() {
	int i = decodeIndsCounter_;
	for (; i < numDrawInds_; i++) {
		const DeferredInds &di = drawInds_[i];

		int indexOffset = drawVertexOffsets_[di.vertDecodeIndex] + di.offset;
		bool clockwise = di.clockwise;
		// We've already collapsed subsequent draws with the same vertex pointer, so no tricky logic here anymore.
		// 2. Loop through the drawcalls, translating indices as we go.
		switch (di.indexType) {
		case GE_VTYPE_IDX_NONE >> GE_VTYPE_IDX_SHIFT:
			indexGen.AddPrim(di.prim, di.vertexCount, indexOffset, clockwise);
			break;
		case GE_VTYPE_IDX_8BIT >> GE_VTYPE_IDX_SHIFT:
			indexGen.TranslatePrim(di.prim, di.vertexCount, (const u8 *)di.inds, indexOffset, clockwise);
			break;
		case GE_VTYPE_IDX_16BIT >> GE_VTYPE_IDX_SHIFT:
			indexGen.TranslatePrim(di.prim, di.vertexCount, (const u16_le *)di.inds, indexOffset, clockwise);
			break;
		case GE_VTYPE_IDX_32BIT >> GE_VTYPE_IDX_SHIFT:
			indexGen.TranslatePrim(di.prim, di.vertexCount, (const u32_le *)di.inds, indexOffset, clockwise);
			break;
		}
	}
	decodeIndsCounter_ = i;

	// Sanity check
	if (indexGen.Prim() < 0) {
		ERROR_LOG_REPORT(G3D, "DecodeVerts: Failed to deduce prim: %i", indexGen.Prim());
		// Force to points (0)
		indexGen.AddPrim(GE_PRIM_POINTS, 0, 0, true);
	}
}

bool DrawEngineCommon::CanUseHardwareTransform(int prim) {
	if (!useHWTransform_)
		return false;
	return !gstate.isModeThrough() && prim != GE_PRIM_RECTANGLES && prim > GE_PRIM_LINE_STRIP;
}

bool DrawEngineCommon::CanUseHardwareTessellation(GEPatchPrimType prim) {
	if (useHWTessellation_) {
		return CanUseHardwareTransform(PatchPrimToPrim(prim));
	}
	return false;
}

void TessellationDataTransfer::CopyControlPoints(float *pos, float *tex, float *col, int posStride, int texStride, int colStride, const SimpleVertex *const *points, int size, u32 vertType) {
	bool hasColor = (vertType & GE_VTYPE_COL_MASK) != 0;
	bool hasTexCoord = (vertType & GE_VTYPE_TC_MASK) != 0;

	for (int i = 0; i < size; ++i) {
		memcpy(pos, points[i]->pos.AsArray(), 3 * sizeof(float));
		pos += posStride;
	}
	if (hasTexCoord) {
		for (int i = 0; i < size; ++i) {
			memcpy(tex, points[i]->uv, 2 * sizeof(float));
			tex += texStride;
		}
	}
	if (hasColor) {
		for (int i = 0; i < size; ++i) {
			memcpy(col, Vec4f::FromRGBA(points[i]->color_32).AsArray(), 4 * sizeof(float));
			col += colStride;
		}
	}
}
