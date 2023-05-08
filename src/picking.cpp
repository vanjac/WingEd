#include "picking.h"
#include <glm/vec4.hpp>
#include "winchroma.h"
#include <glm/gtx/norm.hpp>

namespace winged {

const float FACE_Z_OFFSET = .0001f;
const float VERT_Z_OFFSET = -.0002f;

// returns normalized device coords
glm::vec3 projectPoint(glm::vec3 point, const glm::mat4 &project) {
    // https://stackoverflow.com/a/63084621
    glm::vec4 tv = project * glm::vec4(point, 1);
    return glm::vec3(tv) / tv.w;
}

PickResult pickElement(const Surface &surf, Surface::ElementType types,
        glm::vec2 cursor, glm::vec2 windowDim, const glm::mat4 &project) {
    glm::vec2 normCur = cursor / windowDim * 2.0f - 1.0f; // range -1 to 1
    normCur.y *= -1; // opengl convention

    PickResult result;
    float closestZ = 2; // range -1 to 1

    if (types & Surface::VERT) {
        const glm::vec2 normPointDist = PICK_POINT_SIZE / windowDim;
        for (auto &vert : surf.verts) {
            glm::vec3 normVert = projectPoint(vert.second.pos, project);
            normVert.z += VERT_Z_OFFSET;
            if (glm::all(glm::lessThanEqual(glm::abs(glm::vec2(normVert) - normCur), normPointDist))
                    && glm::abs(normVert.z) <= 1 && normVert.z < closestZ) {
                result = PickResult(Surface::VERT, vert.first, vert.second.pos);
                closestZ = normVert.z;
            }
        }
    }
    if (types == Surface::VERT)
        return result;

    const glm::mat4 invProj = glm::inverse(project);
    glm::vec3 rayOrg = projectPoint(glm::vec3(normCur, -1), invProj); // near plane intersect
    glm::vec3 farPt = projectPoint(glm::vec3(normCur, 1), invProj);
    glm::vec3 rayDir = glm::normalize(farPt - rayOrg);

    if (types & Surface::EDGE) {
        const glm::vec2 normEdgeDist = PICK_EDGE_SIZE / windowDim;
        for (auto &edge : surf.edges) {
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
                if (glm::length2(rejection) != 0) {
                    float t = -glm::length(rejection) / glm::dot(v2-v1, glm::normalize(rejection));
                    t = glm::clamp(t, 0.0f, 1.0f);
                    glm::vec3 point = v1 + t * (v2 - v1);
                    glm::vec3 normPoint = projectPoint(point, project);
                    if (normPoint.z < closestZ
                            && glm::abs(normPoint.x - normCur.x) < normEdgeDist.x
                            && glm::abs(normPoint.y - normCur.y) < normEdgeDist.y) {
                        result = PickResult(Surface::EDGE, edge.first, point);
                        closestZ = normPoint.z;
                    }
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
                normPoint.z += FACE_Z_OFFSET;
                if (normPoint.z < closestZ) {
                    result = PickResult(Surface::FACE, face.first, point);
                    closestZ = normPoint.z;
                }
            }
        }
    }
    return result;
}

} // namespace
