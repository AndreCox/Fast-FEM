#pragma once

#include <vector>
#include <Eigen/Eigen>
#include "node.h"
#include "beam.h"
#include "beam_props.h"
#include <iostream>
#include <cmath>

enum UnitSystem
{
    Metric,
    ImperialFeet,
    ImperialInches
};

class FEMSystem
{
public:
    FEMSystem(std::vector<Node> &n, std::vector<Beam> &s, std::vector<MaterialProfile> &materials, std::vector<BeamProfile> &beam_profiles);
    UnitSystem unit_system = Metric;

    // helpers to convert between internal (SI) units and displayed units
    double lengthToDisplay(double meters) const; // m -> display (m or ft)
    double lengthFromDisplay(double display) const;

    double areaToDisplay(double m2) const;
    double areaFromDisplay(double display) const;

    double inertiaToDisplay(double m4) const;
    double inertiaFromDisplay(double display) const;

    double sectionModulusToDisplay(double m3) const;
    double sectionModulusFromDisplay(double display) const;

    double forceToDisplay(double N) const;
    double forceFromDisplay(double display) const;

    double modulusToDisplay(double Pa) const;
    double modulusFromDisplay(double display) const;

    double stressToDisplay(double Pa) const; // Pa -> MPa or psi etc.
    double stressFromDisplay(double display) const;

    // set unit system and recompute any cached values
    void setUnitSystem(UnitSystem u);

    void generate_constraint_row(Eigen::MatrixXd &C, int row_index, int node_id, const std::vector<int> &free_dof_indices);
    int solve_system();
    void assemble_global_stiffness();

    std::vector<Node> nodes;
    std::vector<Beam> beams;
    std::vector<MaterialProfile> materials_list;
    std::vector<BeamProfile> beam_profiles_list;
    Eigen::MatrixXd global_k_matrix;
    Eigen::VectorXd forces;       // forces in x and y directions [F1x, F1y, F2x, F2y, ...]
    Eigen::VectorXd displacement; // displacements in x and y [u1, v1, u2, v2, ...]
    Eigen::VectorXd reactions;    // reaction forces/moments at DOFs (computed after solve)
    bool debug = false;           // enable verbose printing for debugging
    int total_dof;
    float max_stress;
    float min_stress;
};
