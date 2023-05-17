#include "picking.h"
#include <glm/vec4.hpp>
#include <glm/gtx/norm.hpp>
#include <glm/gtx/intersect.hpp>

namespace winged {

const float PICK_POINT_SIZE = 15;
const float PICK_EDGE_SIZE = 15;
const float EDGE_Z_OFFSET = -.001f;
const float VERT_Z_OFFSET = -.002f;

// returns normalized device coords
glm::vec3 projectPoint(glm::vec3 point, const glm::mat4 &project) {
    // https://stackoverflow.com/a/63084621
    glm::vec4 tv = project * glm::vec4(point, 1);
    return glm::vec3(tv) / tv.w;
}

glm::vec2 screenPosToNDC(glm::vec2 pos, glm::vec2 windowDim) {
    glm::vec2 norm = pos / windowDim * 2.0f - 1.0f; // range -1 to 1
    norm.y *= -1; // opengl convention
    return norm;
}

Ray viewPosToRay(glm::vec2 normPos, const glm::mat4 &project) {
    const glm::mat4 invProj = glm::inverse(project);
    glm::vec3 org = projectPoint(glm::vec3(normPos, -1), invProj); // near plane intersect
    glm::vec3 farPt = projectPoint(glm::vec3(normPos, 1), invProj);
    glm::vec3 dir = glm::normalize(farPt - org);
    return {org, dir};
}

glm::vec3 snapPlanePoint(glm::vec3 point, glm::vec3 planePt, glm::vec3 planeNorm, float grid) {
    if (grid == 0)
        return point;
    int axis = maxAxis(glm::abs(planeNorm));
    int a = (axis + 1) % 3, b = (axis + 2) % 3; // orthogonal axes
    glm::vec3 rounded;
    rounded[a] = glm::round(point[a] / grid) * grid;
    rounded[b] = glm::round(point[b] / grid) * grid;
    glm::vec3 diff = rounded - planePt;
    // solve plane equation:
    rounded[axis] = planePt[axis] -
        (planeNorm[a] * diff[a] + planeNorm[b] * diff[b]) / planeNorm[axis];
    return rounded;
}

bool pickVert(glm::vec3 vertPos, glm::vec2 normCur, glm::vec2 windowDim, const glm::mat4 &project,
        float *depth) {
    const glm::vec2 normPointDist = PICK_POINT_SIZE / windowDim;
    glm::vec3 normVert = projectPoint(vertPos, project);
    normVert.z += VERT_Z_OFFSET;
    if (glm::all(glm::lessThanEqual(glm::abs(glm::vec2(normVert) - normCur), normPointDist))
            && glm::abs(normVert.z) <= 1) {
        if (depth) *depth = normVert.z;
        return true;
    }
    return false;
}

PickResult pickElement(const Surface &surf, PickType types, glm::vec2 normCur,
        glm::vec2 windowDim, const glm::mat4 &project, float grid, float maxDepth) {
    PickResult result;
    result.depth = maxDepth;

    if (types & PICK_VERT) {
        for (auto &vert : surf.verts) {
            float depth;
            if (pickVert(vert.second.pos, normCur, windowDim, project, &depth)
                    && depth < result.depth) {
                result = PickResult(PICK_VERT, vert.first, vert.second.pos, depth);
            }
        }
        if (types == PICK_VERT)
            return result; // skip extra matrix calculations
    }

    Ray ray = viewPosToRay(normCur, project);

    if (types & PICK_EDGE) {
        const glm::vec2 normEdgeDist = PICK_EDGE_SIZE / windowDim;
        for (auto &edge : surf.edges) {
            if (!isPrimary(edge)) continue;
            glm::vec3 v1 = edge.second.vert.in(surf).pos;
            glm::vec3 v2 = edge.second.twin.in(surf).vert.in(surf).pos;
            glm::vec3 normV1 = projectPoint(v1, project), normV2 = projectPoint(v2, project);
            glm::vec2 min = glm::vec2(glm::min(normV1, normV2)) - normEdgeDist;
            glm::vec2 max = glm::vec2(glm::max(normV1, normV2)) + normEdgeDist;
            if (glm::abs(normV1.z) <= 1 && glm::abs(normV2.z) <= 1
                    && normCur.x >= min.x && normCur.x <= max.x
                    && normCur.y >= min.y && normCur.y <= max.y) {
                // https://math.stackexchange.com/a/3436386
                // see also: https://stackoverflow.com/q/2316490/11525734
                glm::vec3 lineDir = glm::normalize(v2 - v1);
                glm::vec3 cDir = glm::normalize(glm::cross(lineDir, ray.dir));
                glm::vec3 oDiff = v1 - ray.org;
                glm::vec3 projection = glm::dot(oDiff, ray.dir) * ray.dir;
                glm::vec3 rejection = oDiff - projection - glm::dot(oDiff, cDir) * cDir;
                if (glm::length2(rejection) == 0)
                    continue;
                glm::vec3 vDiff = v2 - v1;
                float t = -glm::length(rejection) / glm::dot(vDiff, glm::normalize(rejection));
                t = glm::clamp(t, 0.0f, 1.0f);
                glm::vec3 point = v1 + t * vDiff;
                glm::vec3 normPoint = projectPoint(point, project);
                normPoint.z += EDGE_Z_OFFSET;
                if (glm::abs(normPoint.z) <= 1 && normPoint.z < result.depth
                        && glm::abs(normPoint.x - normCur.x) < normEdgeDist.x
                        && glm::abs(normPoint.y - normCur.y) < normEdgeDist.y) {
                    if (grid != 0) {
                        int axis = maxAxis(glm::abs(vDiff));
                        float rounded = glm::round(point[axis] / grid) * grid;
                        t = glm::clamp((rounded - v1[axis]) / vDiff[axis], 0.0f, 1.0f);
                        point = v1 + t * vDiff; // DON'T update normPoint (preserve depth)
                    }
                    if (t == 0 && (types & PICK_VERT))
                        result = PickResult(PICK_VERT, edge.second.vert, point, normPoint.z);
                    else if (t == 1 && (types & PICK_VERT))
                        result = PickResult(PICK_VERT, edge.second.twin.in(surf).vert, point,
                            normPoint.z);
                    else
                        result = PickResult(PICK_EDGE, edge.first, point, normPoint.z);
                }
            }
        }
    }
    if (types & PICK_FACE) {
        for (auto &face : surf.faces) {
            // TODO: only loop through face edges once
            glm::vec3 normal = faceNormalNonUnit(surf, face.second);
            if (glm::dot(ray.dir, normal) >= 0)
                continue;

            bool inside = false;
            glm::vec3 prevVertPos = face.second.edge.in(surf).prev.in(surf).vert.in(surf).pos;
            glm::vec3 last = projectPoint(prevVertPos, project);
            for (auto &faceEdge : FaceEdges(surf, face.second)) {
                glm::vec3 vert = projectPoint(faceEdge.second.vert.in(surf).pos, project);
                if (glm::abs(vert.z) > 1) {
                    inside = false;
                    break;
                }
                // count intersections with horizontal ray
                // thank you Arguru
                if (((vert.y <= normCur.y && normCur.y < last.y)
                        || (last.y <= normCur.y && normCur.y < vert.y))
                        && (normCur.x < (last.x-vert.x)*(normCur.y-vert.y)/(last.y-vert.y)+vert.x))
			        inside = !inside;
                last = vert;
            }
            float t;
            if (inside && glm::intersectRayPlane(ray.org, ray.dir, prevVertPos, normal, /*out*/t)) {
                glm::vec3 point = ray.org + t * ray.dir;
                glm::vec3 normPoint = projectPoint(point, project);
                if (normPoint.z < result.depth) {
                    // DON'T update normPoint (preserve depth)
                    point = snapPlanePoint(point, prevVertPos, normal, grid);
                    // TODO: constrain to edge/vertex if outside face boundary!
                    result = PickResult(PICK_FACE, face.first, point, normPoint.z);
                }
            }
        }
    }
    return result;
}

} // namespace
