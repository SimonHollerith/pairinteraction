/*
 * Copyright (c) 2016 Sebastian Weber, Henri Menke. All rights reserved.
 *
 * This file is part of the pairinteraction library.
 *
 * The pairinteraction library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * The pairinteraction library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the pairinteraction library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "SystemOne.hpp"
#include "dtypes.hpp"

#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

SystemOne::SystemOne(std::string species, MatrixElementCache &cache)
    : SystemBase(cache), efield({{0, 0, 0}}), bfield({{0, 0, 0}}), diamagnetism(true), charge(0),
      ordermax(0), distance(std::numeric_limits<double>::max()), species(std::move(species)),
      sym_reflection(NA), sym_rotation({static_cast<float>(ARB)}) {}

SystemOne::SystemOne(std::string species, MatrixElementCache &cache, bool memory_saving)
    : SystemBase(cache, memory_saving), efield({{0, 0, 0}}), bfield({{0, 0, 0}}),
      diamagnetism(true), charge(0), ordermax(0), distance(std::numeric_limits<double>::max()),
      species(std::move(species)), sym_reflection(NA), sym_rotation({static_cast<float>(ARB)}) {}

const std::string &SystemOne::getSpecies() const { return species; }

void SystemOne::setEfield(std::array<double, 3> field) {
    this->onParameterChange();
    efield = field;

    // Transform the electric field into spherical coordinates
    this->changeToSphericalbasis(efield, efield_spherical);
}

void SystemOne::setBfield(std::array<double, 3> field) {
    this->onParameterChange();
    bfield = field;

    // Transform the magnetic field into spherical coordinates
    this->changeToSphericalbasis(bfield, bfield_spherical);

    diamagnetism_terms[{{0, +0}}] = bfield_spherical[+0] * bfield_spherical[+0] -
        bfield_spherical[+1] * bfield_spherical[-1] * 2.;
    diamagnetism_terms[{{2, +0}}] =
        bfield_spherical[+0] * bfield_spherical[+0] + bfield_spherical[+1] * bfield_spherical[-1];
    diamagnetism_terms[{{2, +1}}] = bfield_spherical[+0] * bfield_spherical[-1];
    diamagnetism_terms[{{2, -1}}] = bfield_spherical[+0] * bfield_spherical[+1];
    diamagnetism_terms[{{2, +2}}] = bfield_spherical[-1] * bfield_spherical[-1];
    diamagnetism_terms[{{2, -2}}] = bfield_spherical[+1] * bfield_spherical[+1];
}

void SystemOne::setEfield(std::array<double, 3> field, std::array<double, 3> to_z_axis,
                          std::array<double, 3> to_y_axis) {
    this->rotateVector(field, to_z_axis, to_y_axis);
    this->setEfield(field);
}

void SystemOne::setBfield(std::array<double, 3> field, std::array<double, 3> to_z_axis,
                          std::array<double, 3> to_y_axis) {
    this->rotateVector(field, to_z_axis, to_y_axis);
    this->setBfield(field);
}

void SystemOne::setEfield(std::array<double, 3> field, double alpha, double beta, double gamma) {
    this->rotateVector(field, alpha, beta, gamma);
    this->setEfield(field);
}

void SystemOne::setBfield(std::array<double, 3> field, double alpha, double beta, double gamma) {
    this->rotateVector(field, alpha, beta, gamma);
    this->setBfield(field);
}

void SystemOne::enableDiamagnetism(bool enable) {
    this->onParameterChange();
    diamagnetism = enable;
}
void SystemOne::setIonCharge(int c) {
    this->onParameterChange();
    charge = c;
}

void SystemOne::setRydIonOrder(unsigned int o) {
    this->onParameterChange();
    ordermax = o;
}

void SystemOne::setRydIonDistance(double d) {
    this->onParameterChange();
    distance = d;
}

void SystemOne::setConservedParityUnderReflection(parity_t parity) {
    this->onSymmetryChange();
    sym_reflection = parity;
    if (!this->isRefelectionAndRotationCompatible()) {
        throw std::runtime_error("The conserved parity under reflection is not compatible to the "
                                 "previously specified conserved momenta.");
    }
}

void SystemOne::setConservedMomentaUnderRotation(const std::set<float> &momenta) {
    if (momenta.count(static_cast<float>(ARB)) != 0 && momenta.size() > 1) {
        throw std::runtime_error(
            "If ARB (=arbitrary momentum) is specified, momenta must not be passed explicitely.");
    }
    this->onSymmetryChange();
    sym_rotation = momenta;
    if (!this->isRefelectionAndRotationCompatible()) {
        throw std::runtime_error("The conserved momenta are not compatible to the previously "
                                 "specified conserved parity under reflection.");
    }
}

////////////////////////////////////////////////////////////////////
/// Method that allows base class to initialize Basis //////////////
////////////////////////////////////////////////////////////////////

void SystemOne::initializeBasis() {
    // If the basis is infinite, throw an error
    if (range_n.empty() &&
        (energy_min == std::numeric_limits<double>::lowest() ||
         energy_max == std::numeric_limits<double>::max())) {
        throw std::runtime_error(
            "The number of basis elements is infinite. The basis has to be restricted.");
    }

    ////////////////////////////////////////////////////////////////////
    /// Build one atom states //////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////

    // TODO check whether symmetries are applicable
    // TODO check whether range_j, range_m is half-integer or integer valued

    float s = 0.5;
    if (std::isdigit(species.back()) != 0) {
        s = ((species.back() - '0') - 1) / 2.; // TODO think of a better solution
    }

    size_t idx = 0;
    std::vector<eigen_triplet_t>
        basisvectors_triplets; // TODO reserve states, basisvectors_triplets,
                               // hamiltonian_triplets

    std::vector<eigen_triplet_t> hamiltonian_triplets;

    /// Loop over specified quantum numbers ////////////////////////////

    std::set<int> range_adapted_n, range_adapted_l;
    std::set<float> range_adapted_j, range_adapted_m;

    if (range_n.empty()) {
        throw std::runtime_error(
            "The calculation of range_n via energy restrictions is not yet implemented."); // TODO
    }
    range_adapted_n = range_n;

    for (auto n : range_adapted_n) {

        if (range_l.empty()) {
            this->range(range_adapted_l, 0, n - 1);
        } else {
            range_adapted_l = range_l;
        }
        for (auto l : range_adapted_l) {
            if (l > n - 1 || l < 0) {
                continue;
            }

            if (range_j.empty()) {
                this->range(range_adapted_j, std::fabs(l - s), l + s);
            } else {
                range_adapted_j = range_j;
            }
            for (auto j : range_adapted_j) {
                if (std::fabs(j - l) > s || j < 0) {
                    continue;
                }

                double energy = StateOne(species, n, l, j, s).getEnergy(cache);
                if (!checkIsEnergyValid(energy)) {
                    continue;
                }

                if (range_m.empty()) {
                    this->range(range_adapted_m, -j, j);
                } else {
                    range_adapted_m = range_m;
                }

                // Consider rotation symmetry
                std::set<float> range_allowed_m;
                if (sym_rotation.count(static_cast<float>(ARB)) == 0) {
                    std::set_intersection(sym_rotation.begin(), sym_rotation.end(),
                                          range_adapted_m.begin(), range_adapted_m.end(),
                                          std::inserter(range_allowed_m, range_allowed_m.begin()));
                } else {
                    range_allowed_m = range_adapted_m;
                }

                for (auto m : range_allowed_m) {
                    if (std::fabs(m) > j) {
                        continue;
                    }

                    // Create state
                    StateOne state(species, n, l, j, m);

                    // Check whether reflection symmetry can be realized with the states available
                    if (sym_reflection != NA && state.getM() != 0 &&
                        range_allowed_m.count(-state.getM()) == 0) {
                        throw std::runtime_error("The momentum " + std::to_string(-state.getM()) +
                                                 " required by symmetries cannot be found.");
                    }

                    // Add symmetrized basis vectors
                    this->addSymmetrizedBasisvectors(state, idx, energy, basisvectors_triplets,
                                                     hamiltonian_triplets, sym_reflection);
                }
            }
        }
    }

    /// Loop over user-defined states //////////////////////////////////

    // Check that the user-defined states are not already contained in the list of states
    for (const auto &state : states_to_add) {
        if (states.get<1>().find(state) != states.get<1>().end()) {
            throw std::runtime_error("The state " + state.str() +
                                     " is already contained in the list of states.");
        }
        if (!state.isArtificial() && state.getSpecies() != species) {
            throw std::runtime_error("The state " + state.str() + " is of the wrong species.");
        }
    }

    // Add user-defined states
    for (const auto &state : states_to_add) {
        // Get energy of the state
        double energy = state.isArtificial() ? 0 : state.getEnergy(cache);

        // In case of artificial states, symmetries won't work
        auto sym_reflection_local = sym_reflection;
        auto sym_rotation_local = sym_rotation;
        if (state.isArtificial()) {
            if (sym_reflection_local != NA ||
                sym_rotation_local.count(static_cast<float>(ARB)) == 0) {
                std::cerr
                    << "WARNING: Only permutation symmetry can be applied to artificial states."
                    << std::endl;
            }
            sym_reflection_local = NA;
            sym_rotation_local = std::set<float>({static_cast<float>(ARB)});
        }

        // Consider rotation symmetry
        if (sym_rotation_local.count(static_cast<float>(ARB)) == 0 &&
            sym_rotation_local.count(state.getM()) == 0) {
            continue;
        }

        // Check whether reflection symmetry can be realized with the states available
        if (sym_reflection_local != NA && state.getM() != 0) {
            auto state_reflected = state.getReflected();
            if (states_to_add.find(state_reflected) == states_to_add.end()) {
                throw std::runtime_error("The state " + state_reflected.str() +
                                         " required by symmetries cannot be found.");
            }
        }

        // Add symmetrized basis vectors
        this->addSymmetrizedBasisvectors(state, idx, energy, basisvectors_triplets,
                                         hamiltonian_triplets, sym_reflection_local);
    }

    /// Build data /////////////////////////////////////////////////////

    basisvectors.resize(states.size(), idx);
    basisvectors.setFromTriplets(basisvectors_triplets.begin(), basisvectors_triplets.end());
    basisvectors_triplets.clear();

    hamiltonian.resize(idx, idx);
    hamiltonian.setFromTriplets(hamiltonian_triplets.begin(), hamiltonian_triplets.end());
    hamiltonian_triplets.clear();
}

////////////////////////////////////////////////////////////////////
/// Method that allows base class to calculate the interaction /////
////////////////////////////////////////////////////////////////////

void SystemOne::initializeInteraction() {
    ////////////////////////////////////////////////////////////////////
    /// Prepare the calculation of the interaction /////////////////////
    ////////////////////////////////////////////////////////////////////

    // Check if something to do
    double tolerance = 1e-24;

    std::vector<int> erange, brange;
    std::vector<std::array<int, 2>> drange;
    std::vector<int> orange;
    for (const auto &entry : efield_spherical) {
        if (entry.first < 0) {
            continue;
        }
        if (std::abs(entry.second) > tolerance &&
            interaction_efield.find(-entry.first) == interaction_efield.end()) {
            erange.push_back(entry.first);
        }
    }
    for (const auto &entry : bfield_spherical) {
        if (entry.first < 0) {
            continue;
        }
        if (std::abs(entry.second) > tolerance &&
            interaction_bfield.find(-entry.first) == interaction_bfield.end()) {
            brange.push_back(entry.first);
        }
    }
    for (const auto &entry : diamagnetism_terms) {
        if (entry.first[1] < 0) {
            continue;
        }
        if (diamagnetism && std::abs(entry.second) > tolerance &&
            interaction_diamagnetism.find(entry.first) == interaction_diamagnetism.end()) {
            drange.push_back(entry.first);
        }
    }

    if (charge != 0) {
        for (unsigned int order = 1; order <= ordermax; ++order) {
            if (interaction_multipole.find(order) == interaction_multipole.end()) {
                orange.push_back(order);
            }
        }
    }
    // Return if there is nothing to do
    if (erange.empty() && brange.empty() && drange.empty() && orange.empty()) {
        return;
    }

    // Precalculate matrix elements
    auto states_converted = this->getStates();
    for (const auto &i : erange) {
        cache.precalculateElectricMomentum(states_converted, i);
        if (i != 0) {
            cache.precalculateElectricMomentum(states_converted, -i);
        }
    }
    for (const auto &i : brange) {
        cache.precalculateMagneticMomentum(states_converted, i);
        if (i != 0) {
            cache.precalculateMagneticMomentum(states_converted, -i);
        }
    }
    for (const auto &i : drange) {
        cache.precalculateDiamagnetism(states_converted, i[0], i[1]);
        if (i[1] != 0) {
            cache.precalculateDiamagnetism(states_converted, i[0], -i[1]);
        }
    }
    if (charge != 0) {
        for (unsigned int order = 1; order <= ordermax; ++order) {
            cache.precalculateMultipole(states_converted, order);
        }
    }

    ////////////////////////////////////////////////////////////////////
    /// Calculate the interaction in the canonical basis ///////////////
    ////////////////////////////////////////////////////////////////////

    std::unordered_map<int, std::vector<eigen_triplet_t>>
        interaction_efield_triplets; // TODO reserve
    std::unordered_map<int, std::vector<eigen_triplet_t>>
        interaction_bfield_triplets; // TODO reserve
    std::unordered_map<std::array<int, 2>, std::vector<eigen_triplet_t>,
                       utils::hash<std::array<int, 2>>>
        interaction_diamagnetism_triplets; // TODO reserve
    std::unordered_map<int, std::vector<eigen_triplet_t>>
        interaction_multipole_triplets; // TODO reserve
    // Loop over column entries
    for (const auto &c : states) { // TODO parallelization
        if (c.state.isArtificial()) {
            continue;
        }

        // Loop over row entries
        for (const auto &r : states) {
            if (r.state.isArtificial()) {
                continue;
            }

            // E-field interaction
            for (const auto &i : erange) {
                if (i == 0 && r.idx < c.idx) {
                    continue;
                }

                if (selectionRulesMultipoleNew(r.state, c.state, 1, i)) {
                    scalar_t value = cache.getElectricDipole(r.state, c.state);
                    this->addTriplet(interaction_efield_triplets[i], r.idx, c.idx, value);
                    break; // because for the other operators, the selection rule for the magnetic
                           // quantum numbers will not be fulfilled
                }
            }

            // B-field interaction
            for (const auto &i : brange) {
                if (i == 0 && r.idx < c.idx) {
                    continue;
                }

                if (selectionRulesMomentumNew(r.state, c.state, i)) {
                    scalar_t value = cache.getMagneticDipole(r.state, c.state);
                    this->addTriplet(interaction_bfield_triplets[i], r.idx, c.idx, value);
                    break; // because for the other operators, the selection rule for the magnetic
                           // quantum numbers will not be fulfilled
                }
            }

            // Diamagnetic interaction
            for (const auto &i : drange) {
                if (i[1] == 0 && r.idx < c.idx) {
                    continue;
                }

                if (selectionRulesMultipoleNew(r.state, c.state, i[0], i[1])) {
                    scalar_t value = 1. / (8 * electron_rest_mass) *
                        cache.getDiamagnetism(r.state, c.state, i[0]);
                    this->addTriplet(interaction_diamagnetism_triplets[i], r.idx, c.idx, value);
                }
            }

            // Multipole interaction with an ion
            if (charge != 0) {
                int q = r.state.getM() - c.state.getM();
                if (q == 0) { // total momentum consreved
                    for (const auto &order : orange) {
                        if (selectionRulesMultipoleNew(r.state, c.state, order)) {
                            double val = -coulombs_constant * elementary_charge *
                                cache.getElectricMultipole(r.state, c.state, order);
                            this->addTriplet(interaction_multipole_triplets[order], r.idx, c.idx,
                                             val);
                        }
                    }
                }
            }
        }
    }
    ////////////////////////////////////////////////////////////////////
    /// Build and transform the interaction to the used basis //////////
    ////////////////////////////////////////////////////////////////////

    for (const auto &i : erange) {
        interaction_efield[i].resize(states.size(), states.size());
        interaction_efield[i].setFromTriplets(interaction_efield_triplets[i].begin(),
                                              interaction_efield_triplets[i].end());
        interaction_efield_triplets[i].clear();

        if (i == 0) {
            interaction_efield[i] = basisvectors.adjoint() *
                interaction_efield[i].selfadjointView<Eigen::Lower>() * basisvectors;
        } else {
            interaction_efield[i] = basisvectors.adjoint() * interaction_efield[i] * basisvectors;
            interaction_efield[-i] = std::pow(-1, i) * interaction_efield[i].adjoint();
        }
    }

    for (const auto &i : brange) {
        interaction_bfield[i].resize(states.size(), states.size());
        interaction_bfield[i].setFromTriplets(interaction_bfield_triplets[i].begin(),
                                              interaction_bfield_triplets[i].end());
        interaction_bfield_triplets[i].clear();

        if (i == 0) {
            interaction_bfield[i] = basisvectors.adjoint() *
                interaction_bfield[i].selfadjointView<Eigen::Lower>() * basisvectors;
        } else {
            interaction_bfield[i] = basisvectors.adjoint() * interaction_bfield[i] * basisvectors;
            interaction_bfield[-i] = std::pow(-1, i) * interaction_bfield[i].adjoint();
        }
    }

    for (const auto &i : drange) {
        interaction_diamagnetism[i].resize(states.size(), states.size());
        interaction_diamagnetism[i].setFromTriplets(interaction_diamagnetism_triplets[i].begin(),
                                                    interaction_diamagnetism_triplets[i].end());
        interaction_diamagnetism_triplets[i].clear();

        if (i[1] == 0) {
            interaction_diamagnetism[i] = basisvectors.adjoint() *
                interaction_diamagnetism[i].selfadjointView<Eigen::Lower>() * basisvectors;
        } else {
            interaction_diamagnetism[i] =
                basisvectors.adjoint() * interaction_diamagnetism[i] * basisvectors;
            interaction_diamagnetism[{{i[0], -i[1]}}] =
                std::pow(-1, i[1]) * interaction_diamagnetism[i].adjoint();
        }
    }
    if (charge != 0) {
        for (const auto &i : orange) {
            interaction_multipole[i].resize(states.size(), states.size());
            interaction_multipole[i].setFromTriplets(interaction_multipole_triplets[i].begin(),
                                                     interaction_multipole_triplets[i].end());
            interaction_multipole_triplets[i].clear();
            if (i == 0) {
                interaction_multipole[i] = basisvectors.adjoint() *
                    interaction_multipole[i].selfadjointView<Eigen::Lower>() * basisvectors;
            } else {
                interaction_multipole[i] =
                    basisvectors.adjoint() * interaction_multipole[i] * basisvectors;
                interaction_multipole[-i] = std::pow(-1, i) * interaction_multipole[i].adjoint();
            }
        }
    }
}

////////////////////////////////////////////////////////////////////
/// Method that allows base class to construct Hamiltonian /////////
////////////////////////////////////////////////////////////////////

void SystemOne::addInteraction() {
    // Build the total Hamiltonian
    double tolerance = 1e-24;

    if (std::abs(efield_spherical[+0]) > tolerance) {
        hamiltonian -= interaction_efield[+0] * efield_spherical[+0];
    }
    if (std::abs(efield_spherical[-1]) > tolerance) {
        hamiltonian += interaction_efield[+1] * efield_spherical[-1];
    }
    if (std::abs(efield_spherical[+1]) > tolerance) {
        hamiltonian += interaction_efield[-1] * efield_spherical[+1];
    }
    if (std::abs(bfield_spherical[+0]) > tolerance) {
        hamiltonian -= interaction_bfield[+0] * bfield_spherical[+0];
    }
    if (std::abs(bfield_spherical[-1]) > tolerance) {
        hamiltonian += interaction_bfield[+1] * bfield_spherical[-1];
    }
    if (std::abs(bfield_spherical[+1]) > tolerance) {
        hamiltonian += interaction_bfield[-1] * bfield_spherical[+1];
    }

    if (diamagnetism && std::abs(diamagnetism_terms[{{0, +0}}]) > tolerance) {
        hamiltonian += interaction_diamagnetism[{{0, +0}}] * diamagnetism_terms[{{0, +0}}];
    }
    if (diamagnetism && std::abs(diamagnetism_terms[{{2, +0}}]) > tolerance) {
        hamiltonian -= interaction_diamagnetism[{{2, +0}}] * diamagnetism_terms[{{2, +0}}];
    }
    if (diamagnetism && std::abs(diamagnetism_terms[{{2, +1}}]) > tolerance) {
        hamiltonian +=
            interaction_diamagnetism[{{2, +1}}] * diamagnetism_terms[{{2, +1}}] * std::sqrt(3);
    }
    if (diamagnetism && std::abs(diamagnetism_terms[{{2, -1}}]) > tolerance) {
        hamiltonian +=
            interaction_diamagnetism[{{2, -1}}] * diamagnetism_terms[{{2, -1}}] * std::sqrt(3);
    }
    if (diamagnetism && std::abs(diamagnetism_terms[{{2, +2}}]) > tolerance) {
        hamiltonian -=
            interaction_diamagnetism[{{2, +2}}] * diamagnetism_terms[{{2, +2}}] * std::sqrt(1.5);
    }
    if (diamagnetism && std::abs(diamagnetism_terms[{{2, -2}}]) > tolerance) {
        hamiltonian -=
            interaction_diamagnetism[{{2, -2}}] * diamagnetism_terms[{{2, -2}}] * std::sqrt(1.5);
    }
    if (charge != 0 && distance != std::numeric_limits<double>::max()) {
        for (unsigned int order = 1; order <= ordermax; ++order) {
            double powerlaw = 1. / std::pow(distance, order + 1);
            hamiltonian += interaction_multipole[order] * charge * powerlaw;
        }
    }
}

////////////////////////////////////////////////////////////////////
/// Method that allows base class to transform the interaction /////
////////////////////////////////////////////////////////////////////

void SystemOne::transformInteraction(const eigen_sparse_t &transformator) {
    for (auto &entry : interaction_efield) {
        entry.second = transformator.adjoint() * entry.second * transformator;
    }
    for (auto &entry : interaction_bfield) {
        entry.second = transformator.adjoint() * entry.second * transformator;
    }
    for (auto &entry : interaction_diamagnetism) {
        entry.second = transformator.adjoint() * entry.second * transformator; // NOLINT
    }
    for (auto &entry : interaction_multipole) {
        entry.second = transformator.adjoint() * entry.second * transformator; // NOLINT
    }
}

////////////////////////////////////////////////////////////////////
/// Method that allows base class to delete the interaction ////////
////////////////////////////////////////////////////////////////////

void SystemOne::deleteInteraction() {
    interaction_efield.clear();
    interaction_bfield.clear();
    interaction_diamagnetism.clear();
    interaction_multipole.clear();
}

////////////////////////////////////////////////////////////////////
/// Methods that allows base class to rotate states ////////////////
////////////////////////////////////////////////////////////////////

eigen_sparse_t SystemOne::rotateStates(const std::vector<size_t> &states_indices, double alpha,
                                       double beta, double gamma) {
    // Initialize Wigner D matrix
    WignerD wigner;

    // Rotate state
    std::vector<eigen_triplet_t> states_rotated_triplets;
    states_rotated_triplets.reserve(
        std::min(static_cast<size_t>(10), states.size()) *
        states_indices.size()); // TODO std::min( 2*jmax+1, states.size() ) * states_indices.size()

    size_t current = 0;
    for (auto const &idx : states_indices) {
        this->addRotated(states[idx].state, current++, states_rotated_triplets, wigner, alpha, beta,
                         gamma);
    }

    eigen_sparse_t states_rotated(states.size(), states_indices.size());
    states_rotated.setFromTriplets(states_rotated_triplets.begin(), states_rotated_triplets.end());
    states_rotated_triplets.clear();

    return states_rotated;
}

eigen_sparse_t SystemOne::buildStaterotator(double alpha, double beta, double gamma) {
    // Initialize Wigner D matrix
    WignerD wigner;

    // Build rotator
    std::vector<eigen_triplet_t> rotator_triplets;
    rotator_triplets.reserve(
        std::min(static_cast<size_t>(10), states.size()) *
        states.size()); // TODO std::min( 2*jmax+1, states.size() ) * states.size()

    for (auto const &entry : states) {
        this->addRotated(entry.state, entry.idx, rotator_triplets, wigner, alpha, beta, gamma);
    }

    eigen_sparse_t rotator(states.size(), states.size());
    rotator.setFromTriplets(rotator_triplets.begin(), rotator_triplets.end()); // NOLINT
    rotator_triplets.clear();

    return rotator;
}

////////////////////////////////////////////////////////////////////
/// Method that allows base class to combine systems ///////////////
////////////////////////////////////////////////////////////////////

void SystemOne::incorporate(SystemBase<StateOne> &system) {
    // Combine parameters
    if (species != dynamic_cast<SystemOne &>(system).species) {
        throw std::runtime_error(
            "The value of the variable 'element' must be the same for both systems.");
    }
    if (efield != dynamic_cast<SystemOne &>(system).efield) {
        throw std::runtime_error("The value of the variable 'distance' must be the same for both "
                                 "systems."); // implies that efield_spherical is the same, too
    }
    if (bfield != dynamic_cast<SystemOne &>(system).bfield) {
        throw std::runtime_error("The value of the variable 'angle' must be the same for both "
                                 "systems."); // implies that
                                              // bfield_spherical
                                              // is the same,
                                              // too
    }
    if (diamagnetism != dynamic_cast<SystemOne &>(system).diamagnetism) {
        throw std::runtime_error(
            "The value of the variable 'ordermax' must be the same for both systems.");
    }

    // Combine symmetries
    unsigned int num_different_symmetries = 0;
    if (sym_reflection != dynamic_cast<SystemOne &>(system).sym_reflection) {
        sym_reflection = NA;
        ++num_different_symmetries;
    }
    if (!(sym_rotation.size() == dynamic_cast<SystemOne &>(system).sym_rotation.size() &&
          std::equal(sym_rotation.begin(), sym_rotation.end(),
                     dynamic_cast<SystemOne &>(system).sym_rotation.begin()))) {
        if (sym_rotation.count(static_cast<float>(ARB)) != 0 ||
            dynamic_cast<SystemOne &>(system).sym_rotation.count(static_cast<float>(ARB)) != 0) {
            sym_rotation = {static_cast<float>(ARB)};
        } else {
            sym_rotation.insert(dynamic_cast<SystemOne &>(system).sym_rotation.begin(),
                                dynamic_cast<SystemOne &>(system).sym_rotation.end());
        }
        ++num_different_symmetries;
    }
    if (num_different_symmetries > 1) {
        std::cerr << "Warning: The systems differ in more than one symmetry. For the combined "
                     "system, the notion of symmetries might be meaningless."
                  << std::endl;
    }

    // Clear cached interaction
    this->deleteInteraction();
}

////////////////////////////////////////////////////////////////////
/// Utility methods ////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void SystemOne::addSymmetrizedBasisvectors(const StateOne &state, size_t &idx, const double &energy,
                                           std::vector<eigen_triplet_t> &basisvectors_triplets,
                                           std::vector<eigen_triplet_t> &hamiltonian_triplets,
                                           parity_t &sym_reflection_local) {
    // In case of reflection symmetry, skip half of the basis vectors
    if (sym_reflection_local != NA && state.getM() != 0) {
        if (state.getM() < 0) {
            return;
        }
    }

    // Store the energy of the unperturbed one atom state
    hamiltonian_triplets.emplace_back(idx, idx, energy);

    // Adapt the normalization if required by symmetries
    scalar_t value = 1;
    if (sym_reflection_local != NA && state.getM() != 0) {
        value /= std::sqrt(2);
    }

    // Add an entry to the current basis vector
    this->addBasisvectors(state, idx, value, basisvectors_triplets);

    // Add further entries to the current basis vector if required by symmetries
    if (sym_reflection_local != NA && state.getM() != 0) {
        value *= std::pow(-1, state.getL() + state.getM() - state.getJ()) *
            utils::imaginary_unit<scalar_t>();
        value *= (sym_reflection_local == EVEN) ? 1 : -1;
        // S_y is invariant under reflection through xz-plane
        // TODO is the s quantum number of importance here?
        this->addBasisvectors(state.getReflected(), idx, value, basisvectors_triplets);
    }

    ++idx;
}

void SystemOne::addBasisvectors(const StateOne &state, const size_t &idx, const scalar_t &value,
                                std::vector<eigen_triplet_t> &basisvectors_triplets) {
    auto state_iter = states.get<1>().find(state);

    size_t row;
    if (state_iter != states.get<1>().end()) {
        row = state_iter->idx;
    } else {
        row = states.size();
        states.push_back(enumerated_state<StateOne>(row, state));
    }

    basisvectors_triplets.emplace_back(row, idx, value);
}

void SystemOne::changeToSphericalbasis(std::array<double, 3> field,
                                       std::unordered_map<int, double> &field_spherical) {
    if (field[1] != 0) {
        throw std::runtime_error(
            "For fields with non-zero y-coordinates, a complex data type is needed.");
    }
    field_spherical[1] = -field[0] / std::sqrt(2);
    field_spherical[-1] = field[0] / std::sqrt(2);
    field_spherical[0] = field[2];
}

void SystemOne::changeToSphericalbasis(
    std::array<double, 3> field, std::unordered_map<int, std::complex<double>> &field_spherical) {
    field_spherical[1] = std::complex<double>(-field[0] / std::sqrt(2), -field[1] / std::sqrt(2));
    field_spherical[-1] = std::complex<double>(field[0] / std::sqrt(2), -field[1] / std::sqrt(2));
    field_spherical[0] = std::complex<double>(field[2], 0);
}

void SystemOne::addTriplet(std::vector<eigen_triplet_t> &triplets, const size_t r_idx,
                           const size_t c_idx, const scalar_t val) {
    triplets.emplace_back(r_idx, c_idx, val);
}

void SystemOne::rotateVector(std::array<double, 3> &field, std::array<double, 3> &to_z_axis,
                             std::array<double, 3> &to_y_axis) {
    auto field_mapped = Eigen::Map<Eigen::Matrix<double, 3, 1>>(&field[0]);

    if (field_mapped.norm() != 0) {
        Eigen::Matrix<double, 3, 3> rotator = this->buildRotator(to_z_axis, to_y_axis);
        field_mapped = rotator.transpose() * field_mapped;
    }
}

void SystemOne::rotateVector(std::array<double, 3> &field, double alpha, double beta,
                             double gamma) {
    auto field_mapped = Eigen::Map<Eigen::Matrix<double, 3, 1>>(&field[0]);

    if (field_mapped.norm() != 0) {
        Eigen::Matrix<double, 3, 3> rotator = this->buildRotator(alpha, beta, gamma);
        field_mapped = rotator.transpose() * field_mapped;
    }
}

bool SystemOne::isRefelectionAndRotationCompatible() {
    if (sym_rotation.count(static_cast<float>(ARB)) != 0 || sym_reflection == NA) {
        return true;
    }

    for (const auto &s : sym_rotation) {
        if (sym_rotation.count(-s) == 0) {
            return false;
        }
    }

    return true;
}
