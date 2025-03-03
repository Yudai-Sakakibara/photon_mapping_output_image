#include "photon-mapper.hpp"

#include <cstdio>
#include <iomanip>

#include <glm/gtx/component_wise.hpp>

#include "../../sampling/sampling.hpp"
#include "../../sampling/sampler.hpp"
#include "../../common/util.hpp"
#include "../../common/work-queue.hpp"
#include "../../common/priority-queue.hpp"
#include "../../common/constants.hpp"
#include "../../material/material.hpp"
#include "../../surface/surface.hpp"
#include "../../ray/interaction.hpp"

#include "../../octree/octree.hpp"
#include "../../octree/linear-octree.hpp"

PhotonMapper::PhotonMapper(const nlohmann::json& j) : Integrator(j)
{
    constexpr bool print = true;

    const nlohmann::json& pm = j.at("photon_map");

    double caustic_factor = pm.at("caustic_factor");
    size_t photon_emissions = pm.at("emissions");

    k_nearest_photons = getOptional(pm, "k_nearest_photons", 50);
    non_caustic_reject = 1.0 / caustic_factor;
    max_node_data = getOptional(pm, "max_photons_per_octree_leaf", 200);
    direct_visualization = getOptional(pm, "direct_visualization", false);

    photon_emissions = static_cast<size_t>(photon_emissions * caustic_factor);

    // Emissions per work
    constexpr size_t EPW = 100000;

    double total_add_flux = 0.0;
    for (const auto& light : scene.emissives)
    {
        total_add_flux += glm::compAdd(light->material->emittance * light->area());
    }

    struct EmissionWork
    {
        EmissionWork() : light_index(0), emissions_offset(0), num_emissions(0), photon_flux(0.0) { }
        EmissionWork(size_t light_index, size_t emissions_offset, size_t num_emissions, const glm::dvec3& photon_flux)
            : light_index(light_index), emissions_offset(emissions_offset), num_emissions(num_emissions), photon_flux(photon_flux) { }

        size_t light_index;
        size_t emissions_offset;
        size_t num_emissions;
        glm::dvec3 photon_flux;
    };

    std::vector<EmissionWork> work_vec;

    for(size_t i = 0; i < scene.emissives.size(); i++)
    {
        auto light = scene.emissives[i];
        glm::dvec3 light_flux = light->material->emittance * light->area();
        double photon_emissions_share = glm::compAdd(light_flux) / total_add_flux;
        size_t num_light_emissions = static_cast<size_t>(photon_emissions * photon_emissions_share);
        glm::dvec3 photon_flux = light_flux / static_cast<double>(num_light_emissions);

        size_t count = 0;
        while (count != num_light_emissions)
        {
            size_t emissions = count + EPW > num_light_emissions ? num_light_emissions - count : EPW;
            work_vec.emplace_back(i, count, emissions, photon_flux);
            count += emissions;
        }
    }

    std::shuffle(work_vec.begin(), work_vec.end(), Random::engine);
    WorkQueue<EmissionWork> work_queue(work_vec);

    caustic_vecs.resize(1);
    global_vecs.resize(1);

    EmissionWork work;
    while (work_queue.getWork(work))
    {
        auto light = scene.emissives[work.light_index];
        Sampler::initiate(static_cast<uint32_t>(work.light_index));
        for (size_t i = 0; i < work.num_emissions; i++)
        {
                        
            Sampler::setIndex(static_cast<uint32_t>(work.emissions_offset + i));

            auto u = Sampler::get<Dim::PM_LIGHT, 4>();
            glm::dvec3 pos = (*light)(u[0], u[1]);
            glm::dvec3 normal = light->normal(pos);
            glm::dvec3 dir = CoordinateSystem::from(Sampling::cosWeightedHemi(u[2], u[3]), normal);

            pos += normal * C::EPSILON;

            emitPhoton(Ray(pos, dir, scene.ior), work.photon_flux);
        }
    }

    if constexpr(print)
    {
        std::printf("\n----------------------------| PHOTON MAPPING PASS |---------------------\n\nTotal number of photon emissions from light sources: %ld\n\n", photon_emissions);
    }

    bool done_constructing_octrees = false;

    size_t num_global_photons = 0;
    size_t num_caustic_photons = 0;

    auto insertAndPop = [](auto& pvec, auto& pmap)
    {
        size_t removed_bytes = 0;
        auto i = pvec.end();
        while (i > pvec.begin())
        {
            if (removed_bytes > (1 << 24))
            {
                pvec.shrink_to_fit();
                i = pvec.end();
                removed_bytes = 0;
            }
            i--;
            pmap.insert(*i);
            i = pvec.erase(i);
            removed_bytes += sizeof(Photon);
        }
        pvec.clear();
        pvec.shrink_to_fit();
    };

    BoundingBox BB = scene.BB();

    // Intermediate octrees that are converted to linear octrees once constructed.
    Octree<Photon> caustic_map_t(BB, max_node_data);
    Octree<Photon> global_map_t(BB, max_node_data);

    num_global_photons += global_vecs[0].size();
    insertAndPop(global_vecs[0], global_map_t);

    num_caustic_photons += caustic_vecs[0].size();
    insertAndPop(caustic_vecs[0], caustic_map_t);

    // Convert octrees to linear array representation
    caustic_map = LinearOctree<Photon>(caustic_map_t);
    global_map  = LinearOctree<Photon>(global_map_t);

    done_constructing_octrees = true;

    if constexpr(print)
    {
        std::printf("Photon maps and numbers of stored photons: \n\n   Global photons: %ld\n  Caustic photons: %ld\n", num_global_photons, num_caustic_photons);
    }
}

void PhotonMapper::emitPhoton(Ray ray, glm::dvec3 flux)
{
    RefractionHistory refraction_history(ray);
    glm::dvec3 bsdf_absIdotN;
    double bsdf_pdf;

    while (true)
    {
        Sampler::shuffle();

        Intersection intersection = scene.intersect(ray);

        if (!intersection)
        {
            return;
        }

        Interaction interaction(intersection, ray, refraction_history.externalIOR(ray));

        // Only spawn photons at locations that can produce non-dirac delta interactions.
        if (!interaction.material->dirac_delta)
        {
            if (ray.dirac_delta)
            {
                caustic_vecs[0].emplace_back(flux, interaction.position, -ray.direction);
            }
            else if(non_caustic_reject > Sampler::get<Dim::PM_REJECT>()[0])
            {
                global_vecs[0].emplace_back(flux / non_caustic_reject, interaction.position, -ray.direction);
            }
        }

        if (!interaction.sampleBSDF(bsdf_absIdotN, bsdf_pdf, ray, true))
        {
            return;
        }

        bsdf_absIdotN /= bsdf_pdf;

        // Based on slide 13 of:
        // https://cgg.mff.cuni.cz/~jaroslav/teaching/2015-npgr010/slides/11%20-%20npgr010-2015%20-%20PM.pdf
        // I.e. reduce survival probability rather than flux to keep flux of spawned photons roughly constant.
        double survive = std::min(glm::compMax(bsdf_absIdotN), 0.95);
        if (survive == 0.0 || survive <= Sampler::get<Dim::ABSORB>()[0])
        {
            return;
        }

        flux *= bsdf_absIdotN / survive;

        refraction_history.update(ray);
    }
}

glm::dvec3 PhotonMapper::sampleRay(Ray ray)
{
    glm::dvec3 radiance(0.0), throughput(1.0);
    RefractionHistory refraction_history(ray);
    glm::dvec3 bsdf_absIdotN;
    LightSample ls;

    while (true)
    {
        Sampler::shuffle();

        Intersection intersection = scene.intersect(ray);

        if (!intersection)
        {
            return radiance;
        }

        Interaction interaction(intersection, ray, refraction_history.externalIOR(ray));

        radiance += Integrator::sampleEmissive(interaction, ls) * throughput;

        if (interaction.dirac_delta)
        {
            if (!ray.dirac_delta && ray.depth != 0)
            {
                return radiance;
            }
            if (!interaction.sampleBSDF(bsdf_absIdotN, ls.bsdf_pdf, ray))
            {
                return radiance;
            }
            throughput *= bsdf_absIdotN / ls.bsdf_pdf;
        }
        else
        {
            radiance += estimateCausticRadiance(interaction) * throughput; // caustics

            if (!direct_visualization && (ray.dirac_delta || ray.depth == 0))
            {
                // Delay global evaluation
                radiance += Integrator::sampleDirect(interaction, ls) * throughput; // direct illumination
                if (!interaction.sampleBSDF(bsdf_absIdotN, ls.bsdf_pdf, ray))
                {
                    return radiance;
                }
                throughput *= bsdf_absIdotN / ls.bsdf_pdf;
            }
            else
            {
                // Global evaluation
                return radiance + estimateGlobalRadiance(interaction) * throughput; // indirect illumination
            }
        }

        if (absorb(ray, throughput))
        {
            return radiance;
        }

        refraction_history.update(ray);
    }
}

glm::dvec3 PhotonMapper::estimateGlobalRadiance(const Interaction& interaction)
{
    PriorityQueue<SearchResult<Photon>> photons;
    global_map.knnSearch(interaction.position, k_nearest_photons, photons);
    if (photons.empty())
    {
        return glm::dvec3(0.0);
    }
    
    double bsdf_pdf;
    glm::dvec3 bsdf_absIdotN;
    glm::dvec3 radiance(0.0);
    for (const auto& p : photons)
    {
        if (interaction.BSDF(bsdf_absIdotN, p.data.dir(), bsdf_pdf))
        {
            radiance += p.data.flux() * bsdf_absIdotN / bsdf_pdf;
        }
    }
    return radiance / (photons.top().distance2 * C::PI);
}

/********************************************************************
Cone filtering method used for sharper caustics. Simplified for k = 1
*********************************************************************/
glm::dvec3 PhotonMapper::estimateCausticRadiance(const Interaction& interaction)
{
    PriorityQueue<SearchResult<Photon>> photons;
    caustic_map.knnSearch(interaction.position, k_nearest_photons, photons);
    if (photons.empty())
    {
        return glm::dvec3(0.0);
    }

    double inv_max_squared_radius = 1.0 / photons.top().distance2;

    double bsdf_pdf;
    glm::dvec3 bsdf_absIdotN;
    glm::dvec3 radiance(0.0);
    for(const auto& p : photons)
    {
        if (interaction.BSDF(bsdf_absIdotN, p.data.dir(), bsdf_pdf))
        {
            double wp = std::max(0.0, 1.0 - std::sqrt(p.distance2 * inv_max_squared_radius));
            radiance += (p.data.flux() * bsdf_absIdotN * wp) / bsdf_pdf;
        }
    }
    return 3.0 * radiance * inv_max_squared_radius * C::INV_PI;
}