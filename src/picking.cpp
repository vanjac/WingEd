#include "picking.h"
#include <glm/vec4.hpp>
#include <glm/gtx/norm.hpp>
#include "mathutil.h"

namespace winged {

const float EDGE_Z_OFFSET = -.001f;
const float VERT_Z_OFFSET = -.002f;

// returns normalized device coords
glm::vec3 projectPoint(glm::vec3 point, const glm::mat4 &project) {
    // https://stackoverflow.com/a/63084621
    glm::vec4 tv = project * glm::vec4(point, 1);
    return glm::vec3(tv) / tv.w;
}

PickResult pickElement(const Surface &surf, Surface::ElementType types, glm::vec2 cursor,
        glm::vec2 windowDim, const glm::mat4 &project, float grid, float maxDepth) {
    glm::vec2 normCur = cursor / windowDim * 2.0f - 1.0f; // range -1 to 1
    normCur.y *= -1; // opengl convention

    PickResult result;
    result.depth = maxDepth;

    if (types & Surface::VERT) {
        const glm::vec2 normPointDist = PICK_POINT_SIZE / windowDim;
        for (auto &vert : surf.verts) {
            glm::vec3 normVert = projectPoint(vert.second.pos, project);
            normVert.z += VERT_Z_OFFSET;
            if (glm::all(glm::lessThanEqual(glm::abs(glm::vec2(normVert) - normCur), normPointDist))
                    && glm::abs(normVert.z) <= 1 && normVert.z < result.depth) {
                result = PickResult(Surface::VERT, vert.first, vert.second.pos, normVert.z);
            }
        }
        if (types == Surface::VERT)
            return result; // skip extra matrix calculations
    }

    const glm::mat4 invProj = glm::inverse(project);
    glm::vec3 rayOrg = projectPoint(glm::vec3(normCur, -1), invProj); // near plane intersect
    glm::vec3 farPt = projectPoint(glm::vec3(normCur, 1), invProj);
    glm::vec3 rayDir = glm::normalize(farPt - rayOrg);

    if (types & Surface::EDGE) {
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
                glm::vec3 cDir = glm::normalize(glm::cross(lineDir, rayDir));
                glm::vec3 oDiff = v1 - rayOrg;
                glm::vec3 projection = glm::dot(oDiff, rayDir) * rayDir;
                glm::vec3 rejection = oDiff - projection - glm::dot(oDiff, cDir) * cDir;
                if (glm::length2(rejection) == 0)
                    continue;
                glm::vec3 vDiff = v2 - v1;
                float t = -glm::length(rejection) / glm::dot(vDiff, glm::normalize(rejection));
                t = glm::clamp(t, 0.0f, 1.0f);
                glm::vec3 point = v1 + t * vDiff;
                glm::vec3 normPoint = projectPoint(point, project);
                normPoint.z += EDGE_Z_OFFSET;
                if (normPoint.z < result.depth
                        && glm::abs(normPoint.x - normCur.x) < normEdgeDist.x
                        && glm::abs(normPoint.y - normCur.y) < normEdgeDist.y) {
                    if (grid != 0) {
                        int axis = maxAxis(glm::abs(vDiff));
                        float rounded = glm::round(point[axis] / grid) * grid;
                        t = glm::clamp((rounded - v1[axis]) / vDiff[axis], 0.0f, 1.0f);
                        point = v1 + t * vDiff; // DON'T update normPoint (preserve depth)
                    }
                    if (t == 0 && (types & Surface::VERT))
                        result = PickResult(Surface::VERT, edge.second.vert, point, normPoint.z);
                    else if (t == 1 && (types & Surface::VERT))
                        result = PickResult(Surface::VERT, edge.second.twin.in(surf).vert, point,
                            normPoint.z);
                    else
                        result = PickResult(Surface::EDGE, edge.first, point, normPoint.z);
                }
            }
        }
    }
    if (types & Surface::FACE) {
        for (auto &face : surf.faces) {
            // TODO: only loop through face edges once
            glm::vec3 normal = faceNormalNonUnit(surf, face.second);
            if (glm::dot(rayDir, normal) >= 0)
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
            if (inside) {
                float t = glm::dot(prevVertPos - rayOrg, normal) / glm::dot(normal, rayDir);
                glm::vec3 point = rayOrg + t * rayDir;
                glm::vec3 normPoint = projectPoint(point, project);
                if (normPoint.z < result.depth) {
                    if (grid != 0) {
                        int axis = maxAxis(glm::abs(normal));
                        int a = (axis + 1) % 3, b = (axis + 2) % 3; // orthogonal axes
                        glm::vec3 rounded;
                        rounded[a] = glm::round(point[a] / grid) * grid;
                        rounded[b] = glm::round(point[b] / grid) * grid;
                        glm::vec3 diff = rounded - prevVertPos;
                        // solve plane equation:
                        rounded[axis] = prevVertPos[axis] -
                            (normal[a] * diff[a] + normal[b] * diff[b]) / normal[axis];
                        point = rounded; // DON'T update normPoint (preserve depth)
                        // TODO: constrain to edge/vertex if outside face boundary!
                    }
                    result = PickResult(Surface::FACE, face.first, point, normPoint.z);
                }
            }
        }
    }
    return result;
}

} // namespace
