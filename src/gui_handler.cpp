#include "gui_handler.h"
#include <imgui-SFML.h>
#include <fstream>
#include "serialization.h"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>

#ifdef _MSC_VER
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

GUIHandler::GUIHandler(FEMSystem &system, GraphicsRenderer &renderer, sf::RenderWindow &window)
    : window(window), fem_system(system), renderer(renderer),
      show_system_controls(true), show_node_editor(false), show_beam_editor(false)
{

    float detected = 1.0f;
    try
    {
        detected = renderer.GetDPIScale(window.getNativeHandle());
    }
    catch (...)
    {
        // fallback to 1.0
        detected = 1.0f;
    }

    applyDPIScale(detected);
}

void GUIHandler::reloadFontsForDPI(float dpiScale)
{
    ImGuiIO &io = ImGui::GetIO();
    float font_size = 16.0f * current_dpi_scale;
    io.Fonts->Clear();

    ImFont *roboto = io.Fonts->AddFontFromFileTTF("resources/fonts/Roboto-Regular.ttf", font_size);
    if (!roboto)
    {
        std::cerr << "Failed to load Roboto\n";
        // fallback to default font
        io.Fonts->AddFontDefault();
    }
    io.Fonts->Build();
    (void)ImGui::SFML::UpdateFontTexture();
}

void GUIHandler::applyDPIScale(float scale)
{
    ImGuiIO &io = ImGui::GetIO();

    // Reset style and font to base, then apply new scale exactly once.
    ImGui::GetStyle() = base_imgui_style;
    ImGui::GetStyle().ScaleAllSizes(scale);

    current_dpi_scale = scale;

    reloadFontsForDPI(scale);

    io.FontGlobalScale = base_font_global_scale;

    std::cout << "DPI Scale set to: " << scale << " (" << (scale * 100.0f) << "%)\n";
}

void GUIHandler::processEvent(const sf::Event &event)
{
    ImGui::SFML::ProcessEvent(window, event);

    if (ImGui::GetIO().WantCaptureKeyboard)
        return;

    using sc = sf::Keyboard::Scancode;

    // Key pressed event
    if (event.is<sf::Event::KeyPressed>())
    {
        const auto &e = event.getIf<sf::Event::KeyPressed>();

        bool ctrl = sf::Keyboard::isKeyPressed(sc::LControl) ||
                    sf::Keyboard::isKeyPressed(sc::RControl);

        sf::Keyboard::Scancode code = e->scancode;

        if (ctrl && code == sc::S)
        {
            request_save_popup = true;
        }
        else if (ctrl && code == sc::O)
        {
            request_load_popup = true;
        }
        else if (ctrl && code == sc::N)
        {
            fem_system.nodes.clear();
            fem_system.beams.clear();
            fem_system.solve_system();
        }
    }
}

void GUIHandler::render()
{
    if (pending_dpi_scale > 0.0f)
    {
        applyDPIScale(pending_dpi_scale);
        pending_dpi_scale = 0.0f; // Reset the flag
    }

    ImGui::SFML::Update(window, deltaClock.restart());

    systemControls();
    nodeEditor();
    beamEditor();
    materialEditor();
    profileEditor();
    visualizationEditor();
    outputEditor();
    handleSavePopup();
    handleLoadPopup();
    handleDPIAdjust();
    helpPage();
    headerBar();
}

void GUIHandler::nodeEditor()
{
    if (!show_node_editor)
        return;

    ImGui::Begin("Node Editor", &show_node_editor, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("Nodes in System:");
    ImGui::Separator();

    bool constraints_changed = false;

    for (int i = 0; i < fem_system.nodes.size(); ++i)
    {
        ImGui::PushID(i);

        auto &editable_node = fem_system.nodes[i];

        ImGui::Text("Node %d", i + 1);

        // Editable position (displayed in current unit system)
        float pos[2] = {static_cast<float>(fem_system.lengthToDisplay(editable_node.position[0])),
                        static_cast<float>(fem_system.lengthToDisplay(editable_node.position[1]))};
        std::string pos_label = std::string("Position (X, Y) [") + (fem_system.unit_system == Metric ? "m" : (fem_system.unit_system == ImperialInches ? "in" : "ft")) + "]";
        if (ImGui::InputFloat2(pos_label.c_str(), pos))
        {
            editable_node.position[0] = static_cast<float>(fem_system.lengthFromDisplay(pos[0]));
            editable_node.position[1] = static_cast<float>(fem_system.lengthFromDisplay(pos[1]));
            constraints_changed = true;
        }

        // Constraint type
        const char *constraint_items[] = {"Free", "Fixed", "Fixed Pin", "Slider"};
        int current_constraint = 0;
        switch (editable_node.constraint_type)
        {
        case Free:
            current_constraint = 0;
            break;
        case Fixed:
            current_constraint = 1;
            break;
        case FixedPin:
            current_constraint = 2;
            break;
        case Slider:
            current_constraint = 3;
            break;
        default:
            current_constraint = 0;
            break;
        }

        if (ImGui::Combo("Constraint", &current_constraint, constraint_items, IM_ARRAYSIZE(constraint_items)))
        {
            switch (current_constraint)
            {
            case 0:
                editable_node.constraint_type = Free;
                break;
            case 1:
                editable_node.constraint_type = Fixed;
                break;
            case 2:
                editable_node.constraint_type = FixedPin;
                break;
            case 3:
                editable_node.constraint_type = Slider;
                break;
            }
            constraints_changed = true;
        }

        // If Slider, allow angle adjustment
        if (editable_node.constraint_type == Slider)
        {
            float angle_deg = editable_node.constraint_angle;
            if (ImGui::SliderFloat("Slider Angle (deg)", &angle_deg, 0.0f, 360.0f))
            {
                editable_node.constraint_angle = angle_deg;
                constraints_changed = true;
            }
        }

        // Remove node button
        ImGui::SameLine();
        if (ImGui::Button("Remove Node"))
        {
            const int removed_index = i;

            // Remove any beams that reference this node
            for (int s = 0; s < static_cast<int>(fem_system.beams.size()); ++s)
            {
                auto &sp = fem_system.beams[s];
                if (sp.nodes[0] == removed_index || sp.nodes[1] == removed_index)
                {
                    fem_system.beams.erase(fem_system.beams.begin() + s);
                    --s;
                }
            }

            // Decrement node indices in remaining beams that were after the removed node
            for (auto &sp : fem_system.beams)
            {
                if (sp.nodes[0] > removed_index)
                    --sp.nodes[0];
                if (sp.nodes[1] > removed_index)
                    --sp.nodes[1];
            }

            fem_system.nodes.erase(fem_system.nodes.begin() + removed_index);

            ImGui::PopID();
            fem_system.solve_system();
            --i;
            continue;
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    // node creation UI
    ImGui::Separator();
    ImGui::Text("Create New Node:");
    static float new_x = 0.0f;
    static float new_y = 0.0f;
    static int new_constraint = 0;
    static float new_angle = 0.0f;
    const char *constraint_items[] = {"Free", "Fixed", "Fixed Pin", "Slider"};

    // New node position inputs shown in currently selected units
    std::string nx_label = std::string("X (") + (fem_system.unit_system == Metric ? "m" : (fem_system.unit_system == ImperialInches ? "in" : "ft")) + ")";
    std::string ny_label = std::string("Y (") + (fem_system.unit_system == Metric ? "m" : (fem_system.unit_system == ImperialInches ? "in" : "ft")) + ")";
    ImGui::InputFloat(nx_label.c_str(), &new_x);
    ImGui::InputFloat(ny_label.c_str(), &new_y);
    ImGui::Combo("Constraint##NewNode", &new_constraint, constraint_items, IM_ARRAYSIZE(constraint_items));
    if (new_constraint == 3) // Slider
    {
        ImGui::SliderFloat("Slider Angle (deg)##NewNode", &new_angle, 0.0f, 360.0f);
    }

    ImGui::SameLine();
    if (ImGui::Button("Add Node"))
    {
        // Map int to enum and construct node. Adjust constructor call if your Node type differs.
        switch (new_constraint)
        {
        case 0:
            fem_system.nodes.emplace_back(static_cast<float>(fem_system.lengthFromDisplay(new_x)), static_cast<float>(fem_system.lengthFromDisplay(new_y)), Free, 0.0f);
            break;
        case 1:
            fem_system.nodes.emplace_back(static_cast<float>(fem_system.lengthFromDisplay(new_x)), static_cast<float>(fem_system.lengthFromDisplay(new_y)), Fixed, 0.0f);
            break;
        case 2:
            fem_system.nodes.emplace_back(static_cast<float>(fem_system.lengthFromDisplay(new_x)), static_cast<float>(fem_system.lengthFromDisplay(new_y)), FixedPin, 0.0f);
            break;
        case 3:
            fem_system.nodes.emplace_back(static_cast<float>(fem_system.lengthFromDisplay(new_x)), static_cast<float>(fem_system.lengthFromDisplay(new_y)), Slider, new_angle);
            break;
        default:
            fem_system.nodes.emplace_back(new_x, new_y, Free, 0.0f);
            break;
        }

        // Re-solve so system matrices/vectors are rebuilt (solve_system should handle resizing state)
        fem_system.solve_system();
    }

    ImGui::End();

    if (constraints_changed)
    {
        fem_system.solve_system();
    }
}

void GUIHandler::beamEditor()
{
    if (!show_beam_editor)
        return;

    ImGui::Begin("Beam Editor", &show_beam_editor, ImGuiWindowFlags_AlwaysAutoResize);

    // --- Prepare node labels for combos (keep strings alive while UI draws) ---
    std::vector<std::string> node_labels;
    node_labels.reserve(fem_system.nodes.size());
    for (size_t i = 0; i < fem_system.nodes.size(); ++i)
        node_labels.push_back("Node " + std::to_string(i + 1));

    std::vector<const char *> node_items;
    node_items.reserve(node_labels.size());
    for (auto &s : node_labels)
        node_items.push_back(s.c_str());

    // --- Prepare profile & material label arrays ---
    std::vector<const char *> profile_items;
    profile_items.reserve(fem_system.beam_profiles_list.size());
    for (auto &profile : fem_system.beam_profiles_list)
        profile_items.push_back(profile.name.c_str());

    std::vector<const char *> material_items;
    material_items.reserve(fem_system.materials_list.size());
    for (auto &mat : fem_system.materials_list)
        material_items.push_back(mat.name.c_str());

    bool beams_changed = false;

    // --- Existing beams ---
    for (int i = 0; i < static_cast<int>(fem_system.beams.size()); ++i)
    {
        ImGui::PushID(i);
        Beam &beam = fem_system.beams[i];

        ImGui::Text("Beam %d", i + 1);

        // Validate endpoints quickly — if invalid, show warning and allow removal
        const int node_count = static_cast<int>(fem_system.nodes.size());
        bool endpoints_valid = (beam.nodes[0] >= 0 && beam.nodes[0] < node_count &&
                                beam.nodes[1] >= 0 && beam.nodes[1] < node_count);

        if (!endpoints_valid)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "Invalid endpoints (node index out of range)");
            if (ImGui::Button("Remove Invalid Beam"))
            {
                fem_system.beams.erase(fem_system.beams.begin() + i);
                beams_changed = true;
                ImGui::PopID();
                --i;
                continue;
            }
            ImGui::Separator();
            ImGui::PopID();
            continue;
        }

        // Node A combo
        int node_a = beam.nodes[0];
        if (!node_items.empty())
        {
            if (ImGui::Combo("Node A", &node_a, node_items.data(), static_cast<int>(node_items.size())))
            {
                beam.nodes[0] = node_a;
                beams_changed = true;
            }
        }
        else
        {
            ImGui::TextDisabled("No nodes available");
        }

        // Node B combo
        int node_b = beam.nodes[1];
        if (!node_items.empty())
        {
            if (ImGui::Combo("Node B", &node_b, node_items.data(), static_cast<int>(node_items.size())))
            {
                beam.nodes[1] = node_b;
                beams_changed = true;
            }
        }

        // Prevent Node A == Node B
        if (beam.nodes[0] == beam.nodes[1])
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Invalid: Node A == Node B");
        }

        // Profile combo (uses index stored in beam.shape_idx)
        int profile_idx = (beam.shape_idx >= 0 && beam.shape_idx < static_cast<int>(fem_system.beam_profiles_list.size()))
                              ? beam.shape_idx
                              : 0;
        if (!profile_items.empty())
        {
            if (ImGui::Combo("Profile", &profile_idx, profile_items.data(), static_cast<int>(profile_items.size())))
            {
                beam.shape_idx = profile_idx;
                beams_changed = true;
            }
        }
        else
        {
            ImGui::TextDisabled("No profiles available");
        }

        // Material combo (uses index stored in beam.material_idx)
        int material_idx = (beam.material_idx >= 0 && beam.material_idx < static_cast<int>(fem_system.materials_list.size()))
                               ? beam.material_idx
                               : 0;
        if (!material_items.empty())
        {
            if (ImGui::Combo("Material", &material_idx, material_items.data(), static_cast<int>(material_items.size())))
            {
                beam.material_idx = material_idx;
                beams_changed = true;
            }
        }
        else
        {
            ImGui::TextDisabled("No materials available");
        }

        // Truss checkbox: toggles whether beam is modeled as a truss (ignores moment of inertia)
        bool is_truss = beam.is_truss;
        if (ImGui::Checkbox("Truss", &is_truss))
        {
            beam.is_truss = is_truss;
            beams_changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(moment of inertia ignored)");

        // Display stress
        ImGui::SameLine();
        ImGui::Text("Stress: %.2f", beam.stress);

        // Remove button
        if (ImGui::Button("Remove Beam"))
        {
            fem_system.beams.erase(fem_system.beams.begin() + i);
            beams_changed = true;
            ImGui::PopID();
            --i;
            continue;
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    // --- Add new beam controls ---
    static int new_node_a = 0;
    static int new_node_b = 0;
    static int new_profile_idx = 0;
    static int new_material_idx = 0;

    if (!node_items.empty())
    {
        ImGui::Text("Create New Beam:");
        ImGui::Combo("New Node A", &new_node_a, node_items.data(), static_cast<int>(node_items.size()));
        ImGui::Combo("New Node B", &new_node_b, node_items.data(), static_cast<int>(node_items.size()));

        if (!profile_items.empty())
            ImGui::Combo("Profile##NewBeam", &new_profile_idx, profile_items.data(), static_cast<int>(profile_items.size()));
        else
            ImGui::TextDisabled("No profiles available");

        if (!material_items.empty())
            ImGui::Combo("Material##NewBeam", &new_material_idx, material_items.data(), static_cast<int>(material_items.size()));
        else
            ImGui::TextDisabled("No materials available");

        // Checkbox to mark new beam as a truss
        static bool new_is_truss = false;
        ImGui::Checkbox("Truss", &new_is_truss);
        ImGui::SameLine();
        ImGui::TextDisabled("(moment of inertia ignored)");

        if (new_node_a == new_node_b)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Node A and B must be different to create a beam.");
        }

        ImGui::SameLine();
        if (ImGui::Button("Add Beam"))
        {
            // Validate before creating
            if (new_node_a != new_node_b &&
                new_node_a >= 0 && new_node_a < static_cast<int>(fem_system.nodes.size()) &&
                new_node_b >= 0 && new_node_b < static_cast<int>(fem_system.nodes.size()) &&
                !profile_items.empty() && !material_items.empty())
            {
                // Constructor assumed Beam(int n1, int n2, int material_idx, int shape_idx)
                fem_system.beams.emplace_back(new_node_a, new_node_b, new_material_idx, new_profile_idx, new_is_truss);
                // If Beam has an 'is_truss' member, set it here. If not, adjust Beam definition accordingly.
                beams_changed = true;
            }
        }
    }
    else
    {
        ImGui::TextDisabled("No nodes available to create beams.");
    }

    ImGui::End();

    if (beams_changed)
    {
        // Sanity-check beams before solving: remove any that are invalid (defensive)
        for (int i = 0; i < static_cast<int>(fem_system.beams.size()); ++i)
        {
            const Beam &s = fem_system.beams[i];
            if (s.nodes[0] < 0 || s.nodes[0] >= static_cast<int>(fem_system.nodes.size()) ||
                s.nodes[1] < 0 || s.nodes[1] >= static_cast<int>(fem_system.nodes.size()) ||
                s.nodes[0] == s.nodes[1] ||
                s.material_idx < 0 || s.material_idx >= static_cast<int>(fem_system.materials_list.size()) ||
                s.shape_idx < 0 || s.shape_idx >= static_cast<int>(fem_system.beam_profiles_list.size()))
            {
                std::cerr << "Removing invalid beam at index " << i << " during validation.\n";
                fem_system.beams.erase(fem_system.beams.begin() + i);
                --i;
            }
        }

        fem_system.solve_system();
    }
}

void GUIHandler::systemControls()
{

    if (!show_system_controls)
        return;

    ImGui::Begin("System Controls", &show_system_controls, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("2D Truss System Solver");
    ImGui::Separator();

    // Show the System Controls content directly (no tabs)
    {
        bool forces_changed = false;
        for (int i = 0; i < fem_system.nodes.size(); ++i)
        {
            if (fem_system.nodes[i].constraint_type == Free || fem_system.nodes[i].constraint_type == Slider)
            {
                // Display forces in the selected unit system
                float fx_internal = static_cast<float>(fem_system.forces(i * 3));
                float fy_internal = static_cast<float>(fem_system.forces(i * 3 + 1));
                float fx = static_cast<float>(fem_system.forceToDisplay(fx_internal));
                float fy = static_cast<float>(fem_system.forceToDisplay(fy_internal));
                ImGui::PushID(i);

                ImGui::Text("Node %d Forces:", i + 1);

                // Fx controls
                ImGui::Text("Fx (%s):", fem_system.unit_system == Metric ? "N" : "lbf");
                ImGui::SameLine();
                if (ImGui::SliderFloat("##FxSlider", &fx, -10000.0f, 10000.0f))
                {
                    fem_system.forces(i * 3) = fem_system.forceFromDisplay(fx);
                    forces_changed = true;
                }
                ImGui::SameLine();
                if (ImGui::InputFloat("##FxInput", &fx))
                {
                    fem_system.forces(i * 3) = fem_system.forceFromDisplay(fx);
                    forces_changed = true;
                }

                // Fy controls
                ImGui::Text("Fy (%s):", fem_system.unit_system == Metric ? "N" : "lbf");
                ImGui::SameLine();
                if (ImGui::SliderFloat("##FySlider", &fy, -10000.0f, 10000.0f))
                {
                    fem_system.forces(i * 3 + 1) = fem_system.forceFromDisplay(fy);
                    forces_changed = true;
                }
                ImGui::SameLine();
                if (ImGui::InputFloat("##FyInput", &fy))
                {
                    fem_system.forces(i * 3 + 1) = fem_system.forceFromDisplay(fy);
                    forces_changed = true;
                }

                ImGui::Separator();
                ImGui::PopID();
            }
        }

        if (forces_changed)
            fem_system.solve_system();

        // Solution display
        ImGui::Text("Solution:");
        for (int i = 0; i < fem_system.nodes.size(); ++i)
        {
            // Convert rotation from radians to degrees for display
            double theta_rad = fem_system.displacement(i * 3 + 2);
            double theta_deg = theta_rad * 180.0 / M_PI; // Assuming M_PI is defined

            double u_disp = fem_system.lengthToDisplay(fem_system.displacement(i * 3));
            double v_disp = fem_system.lengthToDisplay(fem_system.displacement(i * 3 + 1));
            const char *len_unit = (fem_system.unit_system == Metric) ? "m" : (fem_system.unit_system == ImperialInches ? "in" : "ft");

            ImGui::Text("Node %d: u=%.6f %s, v=%.6f %s, theta=%.6f deg", i + 1,
                        u_disp,
                        len_unit,
                        v_disp,
                        len_unit,
                        theta_deg);
        }

        // Beam stresses
        const char *stress_label = (fem_system.unit_system == Metric) ? "MPa" : "psi";
        ImGui::Text("Beam Stresses (%s):", stress_label);
        int index = 1;
        for (const auto &beam : fem_system.beams)
        {
            sf::Color color = renderer.getStressColor(beam.stress, fem_system.min_stress, fem_system.max_stress);
            double stress_disp = fem_system.stressToDisplay(beam.stress);
            ImGui::TextColored(ImVec4(color.r / 255.f, color.g / 255.f, color.b / 255.f, 1.f),
                               "Beam %d: %.2f %s", index++, stress_disp, stress_label);
        }
    }

    ImGui::End();
}

void GUIHandler::outputEditor()
{
    if (!show_output_tab)
        return;

    ImGui::Begin("Output", &show_output_tab, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("System Output / Export");
    ImGui::Separator();

    // Prepare reaction container so CSV export can also access it
    Eigen::VectorXd reactions;
    bool have_reactions = false;

    // Node displacements table
    ImGui::Text("Node Displacements (display units)");
    if (ImGui::BeginTable("nodes_table", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("u");
        ImGui::TableSetupColumn("v");
        ImGui::TableSetupColumn("theta (deg)");
        ImGui::TableSetupColumn("Constraint");
        ImGui::TableHeadersRow();

        for (int i = 0; i < fem_system.nodes.size(); ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i + 1);

            double u = fem_system.lengthToDisplay(fem_system.displacement(i * 3));
            double v = fem_system.lengthToDisplay(fem_system.displacement(i * 3 + 1));
            double theta_deg = fem_system.displacement(i * 3 + 2) * 180.0 / M_PI;

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.6f", u);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.6f", v);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.6f", theta_deg);
            ImGui::TableSetColumnIndex(4);
            const char *cstr = "";
            switch (fem_system.nodes[i].constraint_type)
            {
            case Free:
                cstr = "Free";
                break;
            case Fixed:
                cstr = "Fixed";
                break;
            case FixedPin:
                cstr = "FixedPin";
                break;
            case Slider:
                cstr = "Slider";
                break;
            }
            ImGui::TextUnformatted(cstr);
        }

        ImGui::EndTable();
    }

    ImGui::Separator();

    // Beam stresses table
    ImGui::Text("Beam Stresses");
    if (ImGui::BeginTable("beams_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Nodes");
        ImGui::TableSetupColumn("Stress");
        ImGui::TableSetupColumn("Material/Profile");
        ImGui::TableHeadersRow();

        int idx = 1;
        for (const auto &b : fem_system.beams)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", idx++);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d - %d", b.nodes[0] + 1, b.nodes[1] + 1);
            ImGui::TableSetColumnIndex(2);
            double stress_disp = fem_system.stressToDisplay(b.stress);
            ImGui::Text("%.3f", stress_disp);
            ImGui::TableSetColumnIndex(3);
            std::string mp = "";
            if (b.material_idx >= 0 && b.material_idx < fem_system.materials_list.size())
                mp += fem_system.materials_list[b.material_idx].name;
            mp += "/";
            if (b.shape_idx >= 0 && b.shape_idx < fem_system.beam_profiles_list.size())
                mp += fem_system.beam_profiles_list[b.shape_idx].name;
            ImGui::TextUnformatted(mp.c_str());
        }

        ImGui::EndTable();
    }

    ImGui::Separator();

    // Reaction forces (display units)
    ImGui::Text("Reaction Forces (display units)");
    if (ImGui::BeginTable("reactions_table", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("Rx");
        ImGui::TableSetupColumn("Ry");
        ImGui::TableSetupColumn("Rtheta");
        ImGui::TableHeadersRow();

        if (fem_system.reactions.size() == fem_system.total_dof && fem_system.total_dof > 0)
        {
            reactions = fem_system.reactions;
            have_reactions = true;
        }
        else
        {
            try
            {
                if (fem_system.global_k_matrix.size() > 0 && fem_system.displacement.size() > 0 && fem_system.global_k_matrix.rows() == fem_system.displacement.size())
                {
                    reactions = fem_system.global_k_matrix * fem_system.displacement - fem_system.forces;
                    have_reactions = (reactions.size() == fem_system.displacement.size());
                }
            }
            catch (...)
            {
                have_reactions = false;
            }
        }

        for (int i = 0; i < fem_system.nodes.size(); ++i)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i + 1);

            if (have_reactions)
            {
                double rx = fem_system.forceToDisplay(reactions(i * 3));
                double ry = fem_system.forceToDisplay(reactions(i * 3 + 1));
                double rtheta = reactions(i * 3 + 2);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", rx);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", ry);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.6f", rtheta);
            }
            else
            {
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0");
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("0");
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("0");
            }
        }

        ImGui::EndTable();
    }

    ImGui::Separator();

    // Export CSV UI
    static char outname_buf[512] = "output.csv";
    ImGui::InputText("CSV Filename", outname_buf, sizeof(outname_buf));
    ImGui::SameLine();
    if (ImGui::Button("Export CSV"))
    {
        std::string fname(outname_buf);
        if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".csv")
            fname += ".csv";

        std::ofstream ofs(fname);
        if (!ofs)
        {
            error_msg = "Could not open CSV for writing.";
            save_error = true;
        }
        else
        {
            ofs << "Nodes\n";
            ofs << "Index,u,v,theta_deg,Constraint\n";
            for (int i = 0; i < fem_system.nodes.size(); ++i)
            {
                double u = fem_system.lengthToDisplay(fem_system.displacement(i * 3));
                double v = fem_system.lengthToDisplay(fem_system.displacement(i * 3 + 1));
                double theta_deg = fem_system.displacement(i * 3 + 2) * 180.0 / M_PI;
                const char *cstr = "";
                switch (fem_system.nodes[i].constraint_type)
                {
                case Free:
                    cstr = "Free";
                    break;
                case Fixed:
                    cstr = "Fixed";
                    break;
                case FixedPin:
                    cstr = "FixedPin";
                    break;
                case Slider:
                    cstr = "Slider";
                    break;
                }
                ofs << (i + 1) << "," << u << "," << v << "," << theta_deg << "," << cstr << "\n";
            }

            ofs << "\nBeams\n";
            ofs << "Index,NodeA,NodeB,Stress,Material,Profile\n";
            for (int i = 0; i < fem_system.beams.size(); ++i)
            {
                const auto &b = fem_system.beams[i];
                double stress_disp = fem_system.stressToDisplay(b.stress);
                std::string mat = (b.material_idx >= 0 && b.material_idx < fem_system.materials_list.size()) ? fem_system.materials_list[b.material_idx].name : "";
                std::string prof = (b.shape_idx >= 0 && b.shape_idx < fem_system.beam_profiles_list.size()) ? fem_system.beam_profiles_list[b.shape_idx].name : "";
                ofs << (i + 1) << "," << (b.nodes[0] + 1) << "," << (b.nodes[1] + 1) << "," << stress_disp << "," << mat << "," << prof << "\n";
            }

            ofs << "\nReactions\n";
            ofs << "Node,Rx,Ry,Rtheta\n";
            if (have_reactions)
            {
                for (int i = 0; i < fem_system.nodes.size(); ++i)
                {
                    double rx = fem_system.forceToDisplay(reactions(i * 3));
                    double ry = fem_system.forceToDisplay(reactions(i * 3 + 1));
                    double rtheta = reactions(i * 3 + 2);
                    ofs << (i + 1) << "," << rx << "," << ry << "," << rtheta << "\n";
                }
            }
            else
            {
                for (int i = 0; i < fem_system.nodes.size(); ++i)
                    ofs << (i + 1) << ",0,0,0\n";
            }

            if (!ofs)
            {
                error_msg = "Error writing CSV file.";
                save_error = true;
            }
        }
    }

    ImGui::End();
}

void GUIHandler::handleLoadPopup()
{
    if (request_load_popup)
    {
        ImGui::OpenPopup("Load From");
        request_load_popup = false;
    }

    if (ImGui::BeginPopupModal("Load From", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Filename", filename_buf, sizeof(filename_buf));

        if (ImGui::Button("Load"))
        {
            trigger_load_read = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // Display error message if load failed
    if (load_error)
    {
        ImGui::OpenPopup("Load Error");
    }
    if (ImGui::BeginPopupModal("Load Error", &load_error, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Failed to load file: %s", error_msg.c_str());
        if (ImGui::Button("OK"))
        {
            load_error = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!trigger_load_read)
        return;

    trigger_load_read = false;
    error_msg.clear();

    // check if user put .ffem extension, add if missing
    std::string filename_str(filename_buf);
    if (filename_str.size() < 5 || filename_str.substr(filename_str.size() - 5) != ".ffem")
    {
        filename_str += ".ffem";
        std::strncpy(filename_buf, filename_str.c_str(), sizeof(filename_buf));
        filename_buf[sizeof(filename_buf) - 1] = '\0'; // ensure null termination
    }

    std::ifstream ifs(filename_buf, std::ios::binary);
    if (!ifs)
    {
        error_msg = "Could not open file for reading.";
        load_error = true;
        return;
    }

    // 1. Header/Magic + format version
    std::uint32_t magic = 0;
    ifs.read(reinterpret_cast<char *>(&magic), sizeof(magic));
    if (!ifs || magic != FILE_MAGIC)
    {
        error_msg = "File magic mismatch or read error.";
        load_error = true;
        return;
    }

    // Read next uint32_t and determine whether this is the new-format header
    // or the old-format material count (old format wrote only one uint32_t then continued with material_count).
    std::uint32_t format_version = 1;
    bool material_count_preloaded = false;
    std::uint32_t preloaded_material_count = 0;

    std::uint32_t next_u32 = 0;
    ifs.read(reinterpret_cast<char *>(&next_u32), sizeof(next_u32));
    if (!ifs)
    {
        // Reading failed - malformed or truncated file
        error_msg = "Unexpected EOF after file magic.";
        load_error = true;
        return;
    }

    if (next_u32 == FILE_FORMAT_VERSION)
    {
        // New format: next_u32 is the format version
        format_version = next_u32;

        // If new format, read unit metadata
        uint8_t unit_byte = 1;
        ifs.read(reinterpret_cast<char *>(&unit_byte), sizeof(unit_byte));
        if (!ifs)
        {
            error_msg = "Failed reading unit metadata.";
            load_error = true;
            return;
        }
        if (unit_byte == 0)
            fem_system.setUnitSystem(ImperialFeet);
        else if (unit_byte == 1)
            fem_system.setUnitSystem(Metric);
        else if (unit_byte == 2)
            fem_system.setUnitSystem(ImperialInches);

        // read optional display scales (length, force) written by the saver so the UI can reflect
        double saved_length_scale = 0.0;
        double saved_force_scale = 0.0;
        double saved_visual_force_scale = 500.0;
        double saved_visual_reaction_scale = 500.0;
        ifs.read(reinterpret_cast<char *>(&saved_length_scale), sizeof(saved_length_scale));
        ifs.read(reinterpret_cast<char *>(&saved_force_scale), sizeof(saved_force_scale));
        // read visual scales saved by the GUI so we can restore them
        ifs.read(reinterpret_cast<char *>(&saved_visual_force_scale), sizeof(saved_visual_force_scale));
        ifs.read(reinterpret_cast<char *>(&saved_visual_reaction_scale), sizeof(saved_visual_reaction_scale));
        if (!ifs)
        {
            error_msg = "Failed reading unit scaling metadata.";
            load_error = true;
            return;
        }

        // Apply visual scales to renderer (restore user preferences)
        renderer.forceScale = static_cast<float>(saved_visual_force_scale);
        renderer.reactionScale = static_cast<float>(saved_visual_reaction_scale);
    }
    else
    {
        // Old format: the uint32 we just read is actually 'material_count'
        format_version = 1;
        material_count_preloaded = true;
        preloaded_material_count = next_u32;
    }

    // Clear current system
    fem_system.materials_list.clear();
    fem_system.beam_profiles_list.clear();
    fem_system.nodes.clear();
    fem_system.beams.clear();
    fem_system.forces = Eigen::VectorXd();

    // 2. Material Profiles
    std::uint32_t material_count = 0;
    if (material_count_preloaded)
    {
        material_count = preloaded_material_count;
    }
    else
    {
        ifs.read(reinterpret_cast<char *>(&material_count), sizeof(material_count));
        if (!ifs)
        {
            error_msg = "Failed reading material count.";
            load_error = true;
            return;
        }
    }
    fem_system.materials_list.resize(material_count);
    for (std::uint32_t i = 0; i < material_count; ++i)
    {
        MaterialProfile &m = fem_system.materials_list[i];
        m.name = readString(ifs);
        ifs.read(reinterpret_cast<char *>(&m.youngs_modulus), sizeof(m.youngs_modulus));
        if (!ifs)
        {
            error_msg = "Failed reading material profile " + std::to_string(i);
            load_error = true;
            return;
        }
    }

    // 3. Beam Profiles
    std::uint32_t profile_count = 0;
    ifs.read(reinterpret_cast<char *>(&profile_count), sizeof(profile_count));
    if (!ifs)
    {
        error_msg = "Failed reading beam profile count.";
        load_error = true;
        return;
    }
    fem_system.beam_profiles_list.resize(profile_count);
    for (std::uint32_t i = 0; i < profile_count; ++i)
    {
        BeamProfile &p = fem_system.beam_profiles_list[i];
        p.name = readString(ifs);
        ifs.read(reinterpret_cast<char *>(&p.area), sizeof(p.area));
        ifs.read(reinterpret_cast<char *>(&p.moment_of_inertia), sizeof(p.moment_of_inertia));
        ifs.read(reinterpret_cast<char *>(&p.section_modulus), sizeof(p.section_modulus));
        if (!ifs)
        {
            error_msg = "Failed reading beam profile " + std::to_string(i);
            load_error = true;
            return;
        }
    }

    // 4. Nodes
    std::uint32_t node_count = 0;
    ifs.read(reinterpret_cast<char *>(&node_count), sizeof(node_count));
    if (!ifs)
    {
        error_msg = "Failed reading node count.";
        load_error = true;
        return;
    }
    fem_system.nodes.resize(node_count);
    for (std::uint32_t i = 0; i < node_count; ++i)
    {
        Node &n = fem_system.nodes[i];
        ifs.read(reinterpret_cast<char *>(&n.position[0]), sizeof(float) * 2);
        int32_t type = 0;
        ifs.read(reinterpret_cast<char *>(&type), sizeof(type));
        ifs.read(reinterpret_cast<char *>(&n.constraint_angle), sizeof(n.constraint_angle));
        if (!ifs)
        {
            error_msg = "Failed reading node " + std::to_string(i);
            load_error = true;
            return;
        }
        // validate enum range (defensive)
        if (type < static_cast<int32_t>(Free) || type > static_cast<int32_t>(Slider))
            n.constraint_type = Free;
        else
            n.constraint_type = static_cast<ConstraintType>(type);
    }

    // 5. Beams (now using indices)
    std::uint32_t beam_count = 0;
    ifs.read(reinterpret_cast<char *>(&beam_count), sizeof(beam_count));
    if (!ifs)
    {
        error_msg = "Failed reading beam count.";
        load_error = true;
        return;
    }
    fem_system.beams.resize(beam_count);
    for (std::uint32_t i = 0; i < beam_count; ++i)
    {
        Beam &s = fem_system.beams[i];

        // Nodes indices (int32_t[2])
        int32_t n0 = -1, n1 = -1;
        ifs.read(reinterpret_cast<char *>(&n0), sizeof(n0));
        ifs.read(reinterpret_cast<char *>(&n1), sizeof(n1));
        if (!ifs)
        {
            error_msg = "Failed reading beam node indices for beam " + std::to_string(i);
            load_error = true;
            return;
        }
        s.nodes[0] = n0;
        s.nodes[1] = n1;

        // Stress (float)
        ifs.read(reinterpret_cast<char *>(&s.stress), sizeof(s.stress));
        if (!ifs)
        {
            error_msg = "Failed reading beam stress for beam " + std::to_string(i);
            load_error = true;
            return;
        }

        // Material and Profile Indices (int32_t[2])
        int32_t mat_idx = -1, shape_idx = -1;
        ifs.read(reinterpret_cast<char *>(&mat_idx), sizeof(mat_idx));
        ifs.read(reinterpret_cast<char *>(&shape_idx), sizeof(shape_idx));
        if (!ifs)
        {
            error_msg = "Failed reading beam material/shape indices for beam " + std::to_string(i);
            load_error = true;
            return;
        }

        // TRUSS FLAG (uint8_t): read whether this beam is a truss
        uint8_t is_truss_flag = 0;
        ifs.read(reinterpret_cast<char *>(&is_truss_flag), sizeof(is_truss_flag));
        if (!ifs)
        {
            error_msg = "Failed reading beam truss flag for beam " + std::to_string(i);
            load_error = true;
            return;
        }
        s.is_truss = (is_truss_flag != 0);

        // Validate indices
        if (mat_idx < 0 || mat_idx >= static_cast<int32_t>(fem_system.materials_list.size()) ||
            shape_idx < 0 || shape_idx >= static_cast<int32_t>(fem_system.beam_profiles_list.size()))
        {
            error_msg = "Invalid material/shape index in beam " + std::to_string(i);
            load_error = true;
            return;
        }
        s.material_idx = mat_idx;
        s.shape_idx = shape_idx;

        // validate node indices
        if (s.nodes[0] < 0 || s.nodes[0] >= static_cast<int32_t>(fem_system.nodes.size()) ||
            s.nodes[1] < 0 || s.nodes[1] >= static_cast<int32_t>(fem_system.nodes.size()))
        {
            error_msg = "Invalid node index in beam " + std::to_string(i);
            load_error = true;
            return;
        }
    }

    // 6. Forces (Eigen::VectorXd)
    std::uint32_t fcount = 0;
    ifs.read(reinterpret_cast<char *>(&fcount), sizeof(fcount));
    if (!ifs)
    {
        error_msg = "Failed reading forces count.";
        load_error = true;
        return;
    }

    const size_t expected_forces = fem_system.nodes.size() * 3;
    if (fcount == expected_forces)
    {
        fem_system.forces.resize(fcount);
        if (fcount > 0)
        {
            ifs.read(reinterpret_cast<char *>(fem_system.forces.data()), sizeof(double) * fcount);
            if (!ifs)
            {
                error_msg = "Failed reading full forces data.";
                load_error = true;
                return;
            }
        }
    }
    else
    {
        // mismatch: safer to zero forces rather than trust a mismatched file
        std::cerr << "Warning: forces count in file (" << fcount << ") does not match expected (" << expected_forces << "). Zeroing forces.\n";
        fem_system.forces = Eigen::VectorXd::Zero(expected_forces);

        // Attempt to skip over the saved forces blob if file contains it
        if (fcount > 0)
        {
            // seek forward to skip the raw doubles (best-effort)
            ifs.seekg(static_cast<std::streamoff>(sizeof(double) * fcount), std::ios::cur);
            // ignore seek failure — we already zeroed forces
        }
    }

    // final file-read sanity check
    if (ifs.fail() && !ifs.eof())
    {
        error_msg = "Error occurred during file reading or file truncated.";
        load_error = true;
        return;
    }

    fem_system.total_dof = static_cast<int>(fem_system.nodes.size()) * 3;
    fem_system.displacement = Eigen::VectorXd::Zero(fem_system.total_dof);
    // forces already set (either loaded or zeroed)
    fem_system.solve_system();
    renderer.autoZoomToFit();

    // all done
}

void GUIHandler::handleSavePopup()
{
    if (request_save_popup)
    {
        ImGui::OpenPopup("Save As");
        request_save_popup = false;
    }

    if (ImGui::BeginPopupModal("Save As", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Filename", filename_buf, sizeof(filename_buf));

        if (ImGui::Button("Save"))
        {
            trigger_save_write = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // Display error message if save failed
    if (save_error)
    {
        ImGui::OpenPopup("Save Error");
    }
    if (ImGui::BeginPopupModal("Save Error", &save_error, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Failed to save file: %s", error_msg.c_str());
        if (ImGui::Button("OK"))
        {
            save_error = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!trigger_save_write)
        return;

    trigger_save_write = false;
    error_msg.clear();

    // check if user put .ffem extension, add if missing
    std::string filename_str(filename_buf);
    if (filename_str.size() < 5 || filename_str.substr(filename_str.size() - 5) != ".ffem")
    {
        filename_str += ".ffem";
        std::strncpy(filename_buf, filename_str.c_str(), sizeof(filename_buf));
        filename_buf[sizeof(filename_buf) - 1] = '\0'; // ensure null termination
    }

    std::ofstream ofs(filename_buf, std::ios::binary);
    if (!ofs)
    {
        error_msg = "Could not open file for writing.";
        save_error = true;
        return;
    }

    // 1. Header/Magic + format version
    ofs.write(reinterpret_cast<const char *>(&FILE_MAGIC), sizeof(FILE_MAGIC));
    std::uint32_t fmt_ver = FILE_FORMAT_VERSION;
    ofs.write(reinterpret_cast<const char *>(&fmt_ver), sizeof(fmt_ver));

    // 1.5 Unit metadata: uint8_t (0=Imperial,1=Metric) followed by optional display scales
    // unit_byte: 0 = ImperialFeet, 1 = Metric, 2 = ImperialInches
    uint8_t unit_byte = 1u;
    if (fem_system.unit_system == Metric)
        unit_byte = 1u;
    else if (fem_system.unit_system == ImperialFeet)
        unit_byte = 0u;
    else if (fem_system.unit_system == ImperialInches)
        unit_byte = 2u;
    ofs.write(reinterpret_cast<const char *>(&unit_byte), sizeof(unit_byte));
    // write informative display scales: length (display units per meter) and force (display units per N)
    double length_display_for_1m = fem_system.lengthToDisplay(1.0); // e.g., 1.0 or 3.2808
    double force_display_for_1N = fem_system.forceToDisplay(1.0);   // e.g., 1.0 or 0.2248
    ofs.write(reinterpret_cast<const char *>(&length_display_for_1m), sizeof(length_display_for_1m));
    ofs.write(reinterpret_cast<const char *>(&force_display_for_1N), sizeof(force_display_for_1N));
    // write renderer visual scales so loading restores the user's visualization preferences
    double visual_force_scale = static_cast<double>(renderer.forceScale);
    double visual_reaction_scale = static_cast<double>(renderer.reactionScale);
    ofs.write(reinterpret_cast<const char *>(&visual_force_scale), sizeof(visual_force_scale));
    ofs.write(reinterpret_cast<const char *>(&visual_reaction_scale), sizeof(visual_reaction_scale));

    // 2. Material Profiles
    std::uint32_t material_count = static_cast<std::uint32_t>(fem_system.materials_list.size());
    ofs.write(reinterpret_cast<const char *>(&material_count), sizeof(material_count));
    for (const auto &m : fem_system.materials_list)
    {
        writeString(ofs, m.name);
        ofs.write(reinterpret_cast<const char *>(&m.youngs_modulus), sizeof(m.youngs_modulus));
    }

    // 3. Beam Profiles
    std::uint32_t profile_count = static_cast<std::uint32_t>(fem_system.beam_profiles_list.size());
    ofs.write(reinterpret_cast<const char *>(&profile_count), sizeof(profile_count));
    for (const auto &p : fem_system.beam_profiles_list)
    {
        writeString(ofs, p.name);
        ofs.write(reinterpret_cast<const char *>(&p.area), sizeof(p.area));
        ofs.write(reinterpret_cast<const char *>(&p.moment_of_inertia), sizeof(p.moment_of_inertia));
        ofs.write(reinterpret_cast<const char *>(&p.section_modulus), sizeof(p.section_modulus));
    }

    // 4. Nodes
    std::uint32_t node_count = static_cast<std::uint32_t>(fem_system.nodes.size());
    ofs.write(reinterpret_cast<const char *>(&node_count), sizeof(node_count));
    for (const auto &n : fem_system.nodes)
    {
        ofs.write(reinterpret_cast<const char *>(&n.position[0]), sizeof(float) * 2);
        int32_t type = static_cast<int32_t>(n.constraint_type);
        ofs.write(reinterpret_cast<const char *>(&type), sizeof(type));
        ofs.write(reinterpret_cast<const char *>(&n.constraint_angle), sizeof(n.constraint_angle));
    }

    // 5. Beams (write node indices, stress, material_idx, shape_idx)
    std::uint32_t beam_count = static_cast<std::uint32_t>(fem_system.beams.size());
    ofs.write(reinterpret_cast<const char *>(&beam_count), sizeof(beam_count));
    for (const auto &s : fem_system.beams)
    {
        // Nodes indices (int32_t[2])
        int32_t n0 = s.nodes[0];
        int32_t n1 = s.nodes[1];
        ofs.write(reinterpret_cast<const char *>(&n0), sizeof(n0));
        ofs.write(reinterpret_cast<const char *>(&n1), sizeof(n1));

        // Stress (float)
        ofs.write(reinterpret_cast<const char *>(&s.stress), sizeof(s.stress));

        // Material and Profile Indices (int32_t[2])
        int32_t mat_idx = s.material_idx;
        int32_t shape_idx = s.shape_idx;
        ofs.write(reinterpret_cast<const char *>(&mat_idx), sizeof(mat_idx));
        ofs.write(reinterpret_cast<const char *>(&shape_idx), sizeof(shape_idx));

        // TRUSS FLAG (uint8_t): whether this beam is modeled as a truss
        {
            uint8_t is_truss_flag = s.is_truss ? 1u : 0u;
            ofs.write(reinterpret_cast<const char *>(&is_truss_flag), sizeof(is_truss_flag));
        }
    }

    // 6. Forces (Eigen::VectorXd)
    std::uint32_t fcount = static_cast<std::uint32_t>(fem_system.forces.size());
    ofs.write(reinterpret_cast<const char *>(&fcount), sizeof(fcount));
    if (fcount > 0)
    {
        ofs.write(reinterpret_cast<const char *>(fem_system.forces.data()), sizeof(double) * fcount);
    }

    if (ofs.fail())
    {
        error_msg = "Error occurred during file writing.";
        save_error = true;
        return;
    }

    // success - file closed on destruction of ofs
}

void GUIHandler::profileEditor()
{
    if (!show_profile_editor)
        return;

    ImGui::Begin("Profile Editor", &show_profile_editor, ImGuiWindowFlags_AlwaysAutoResize);

    bool profiles_changed = false;

    // --- Existing profiles ---
    if (fem_system.beam_profiles_list.empty())
    {
        ImGui::TextDisabled("No profiles created.");
    }
    else
    {
        for (int i = 0; i < static_cast<int>(fem_system.beam_profiles_list.size()); ++i)
        {
            ImGui::PushID(i);
            BeamProfile &profile = fem_system.beam_profiles_list[i];

            ImGui::Text("Profile %d: %s", i + 1, profile.name.c_str());

            // --- Editable Area ---
            float area_disp = static_cast<float>(fem_system.areaToDisplay(profile.area));
            std::string area_label = std::string("Area (") + (fem_system.unit_system == Metric ? "m^2" : "ft^2") + ")";
            if (ImGui::InputFloat(area_label.c_str(), &area_disp))
            {
                profile.area = static_cast<float>(fem_system.areaFromDisplay(area_disp));

                for (auto &b : fem_system.beams)
                {
                    if (b.shape_idx == i)
                        fem_system.beam_profiles_list[b.shape_idx].area = profile.area;
                }
                profiles_changed = true;
            }

            // --- Editable Moment of Inertia ---
            float I_disp = static_cast<float>(fem_system.inertiaToDisplay(profile.moment_of_inertia));
            std::string I_label = std::string("Moment of Inertia I (") + (fem_system.unit_system == Metric ? "m^4" : "ft^4") + ")";
            if (ImGui::InputFloat(I_label.c_str(), &I_disp))
            {
                profile.moment_of_inertia = static_cast<float>(fem_system.inertiaFromDisplay(I_disp));

                for (auto &b : fem_system.beams)
                {
                    if (b.shape_idx == i)
                        fem_system.beam_profiles_list[b.shape_idx].moment_of_inertia = profile.moment_of_inertia;
                }
                profiles_changed = true;
            }

            // --- Editable Section Modulus ---
            float S_disp = static_cast<float>(fem_system.sectionModulusToDisplay(profile.section_modulus));
            std::string S_label = std::string("Section Modulus S (") + (fem_system.unit_system == Metric ? "m^3" : "ft^3") + ")";
            if (ImGui::InputFloat(S_label.c_str(), &S_disp))
            {
                profile.section_modulus = static_cast<float>(fem_system.sectionModulusFromDisplay(S_disp));

                for (auto &b : fem_system.beams)
                {
                    if (b.shape_idx == i)
                        fem_system.beam_profiles_list[b.shape_idx].section_modulus = profile.section_modulus;
                }
                profiles_changed = true;
            }

            // Remove profile button
            ImGui::SameLine();
            if (ImGui::Button("Remove Profile"))
            {
                // Remove beams using this profile
                for (int s = 0; s < static_cast<int>(fem_system.beams.size()); ++s)
                {
                    if (fem_system.beams[s].shape_idx == i)
                    {
                        fem_system.beams.erase(fem_system.beams.begin() + s);
                        --s;
                    }
                }

                fem_system.beam_profiles_list.erase(fem_system.beam_profiles_list.begin() + i);

                ImGui::PopID();
                profiles_changed = true;
                --i;
                continue;
            }

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    // --- Profile Creation ---
    ImGui::Separator();
    ImGui::Text("Create New Profile:");

    static char new_profile_name[128] = "";
    static float new_profile_area = 0.1963f;
    static float new_profile_I = 0.005f;
    static float new_profile_S = 0.01f;

    ImGui::InputText("Name", new_profile_name, sizeof(new_profile_name));
    std::string new_area_label = std::string("Area (") + (fem_system.unit_system == Metric ? "m^2" : "ft^2") + ")";
    std::string new_I_label = std::string("Moment of Inertia I (") + (fem_system.unit_system == Metric ? "m^4" : "ft^4") + ")";
    std::string new_S_label = std::string("Section Modulus S (") + (fem_system.unit_system == Metric ? "m^3" : "ft^3") + ")";
    ImGui::InputFloat(new_area_label.c_str(), &new_profile_area);
    ImGui::InputFloat(new_I_label.c_str(), &new_profile_I);
    ImGui::InputFloat(new_S_label.c_str(), &new_profile_S);

    ImGui::SameLine();
    if (ImGui::Button("Add Profile"))
    {
        std::string name_str(new_profile_name);
        bool duplicate = false;

        for (const auto &p : fem_system.beam_profiles_list)
        {
            if (p.name == name_str)
            {
                duplicate = true;
                break;
            }
        }

        if (!name_str.empty() && !duplicate)
        {
            BeamProfile new_p;
            new_p.name = name_str;
            // Convert from displayed units back to internal SI on creation
            new_p.area = static_cast<float>(fem_system.areaFromDisplay(new_profile_area));
            new_p.moment_of_inertia = static_cast<float>(fem_system.inertiaFromDisplay(new_profile_I));
            new_p.section_modulus = static_cast<float>(fem_system.sectionModulusFromDisplay(new_profile_S));

            fem_system.beam_profiles_list.push_back(new_p);

            // Reset fields
            new_profile_name[0] = '\0';
            new_profile_area = 0.1963f;
            new_profile_I = 0.005f;
            new_profile_S = 0.01f;

            profiles_changed = true;
        }
    }

    ImGui::End();

    if (profiles_changed)
    {
        fem_system.solve_system();
    }
}

void GUIHandler::materialEditor()
{
    if (!show_material_editor)
        return;

    ImGui::Begin("Material Editor", &show_material_editor, ImGuiWindowFlags_AlwaysAutoResize);

    bool materials_changed = false;

    // --- Existing materials ---
    if (fem_system.materials_list.empty())
    {
        ImGui::TextDisabled("No materials created.");
    }
    else
    {
        for (int i = 0; i < static_cast<int>(fem_system.materials_list.size()); ++i)
        {
            ImGui::PushID(i);
            MaterialProfile &mat = fem_system.materials_list[i];

            ImGui::Text("Material %d: %s", i + 1, mat.name.c_str());

            // Editable Young's modulus (displayed in selected unit)
            float youngs_disp = static_cast<float>(fem_system.modulusToDisplay(mat.youngs_modulus));
            std::string ym_label = std::string("Young's Modulus (") + (fem_system.unit_system == Metric ? "Pa" : "psi") + ")";
            if (ImGui::InputFloat(ym_label.c_str(), &youngs_disp))
            {
                mat.youngs_modulus = static_cast<float>(fem_system.modulusFromDisplay(youngs_disp));
                // Update all beams using this material index
                for (auto &sp : fem_system.beams)
                {
                    if (sp.material_idx == i)
                        sp.material_idx = i; // Index remains the same, modulus updated via material reference
                }
                materials_changed = true;
            }

            // Remove material button
            ImGui::SameLine();
            if (ImGui::Button("Remove Material"))
            {
                // Remove all beams using this material
                for (int s = 0; s < static_cast<int>(fem_system.beams.size()); ++s)
                {
                    if (fem_system.beams[s].material_idx == i)
                    {
                        fem_system.beams.erase(fem_system.beams.begin() + s);
                        --s;
                    }
                }

                // Erase the material
                fem_system.materials_list.erase(fem_system.materials_list.begin() + i);

                ImGui::PopID();
                materials_changed = true;

                --i; // Adjust loop index after erase
                continue;
            }

            ImGui::Separator();
            ImGui::PopID();
        }
    }

    // --- New material creation ---
    ImGui::Separator();
    ImGui::Text("Create New Material:");
    static char new_mat_name[128] = "";
    static float new_mat_youngs = 30e6f;

    ImGui::InputText("Name", new_mat_name, sizeof(new_mat_name));
    std::string new_ym_label = std::string("Young's Modulus (") + (fem_system.unit_system == Metric ? "Pa" : "psi") + ")";
    ImGui::InputFloat(new_ym_label.c_str(), &new_mat_youngs);

    ImGui::SameLine();
    if (ImGui::Button("Add Material"))
    {
        std::string name_str(new_mat_name);
        bool duplicate = false;
        for (const auto &mat : fem_system.materials_list)
        {
            if (mat.name == name_str)
            {
                duplicate = true;
                break;
            }
        }

        if (!name_str.empty() && !duplicate)
        {
            MaterialProfile new_mat;
            new_mat.name = name_str;
            new_mat.youngs_modulus = static_cast<double>(fem_system.modulusFromDisplay(new_mat_youngs));
            fem_system.materials_list.push_back(new_mat);

            // Reset fields
            new_mat_name[0] = '\0';
            new_mat_youngs = 30e6f;

            materials_changed = true;
        }
    }

    ImGui::End();

    if (materials_changed)
    {
        fem_system.solve_system();
    }
}

void GUIHandler::handleDPIAdjust()
{
    static bool popup_just_opened = false;

    if (request_dpi_adjust)
    {
        request_dpi_adjust = false;
        popup_just_opened = true; // mark that we need to init on next BeginPopupModal
        ImGui::OpenPopup("Adjust DPI Scaling");
    }

    static float dpi_percent = current_dpi_scale * 100.0f;

    if (ImGui::BeginPopupModal("Adjust DPI Scaling", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (popup_just_opened)
        {
            // Initialize only ONCE when popup opens
            float detected = current_dpi_scale;

            try
            {
                detected = renderer.GetDPIScale(window.getNativeHandle());
            }
            catch (...)
            {
            }

            dpi_percent = detected * 100.0f;
            popup_just_opened = false;
        }

        // Close button
        float close_btn_sz = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetWindowWidth() - close_btn_sz - ImGui::GetStyle().FramePadding.x);
        if (ImGui::Button("X"))
            ImGui::CloseCurrentPopup();

        float detected = current_dpi_scale;
        try
        {
            detected = renderer.GetDPIScale(window.getNativeHandle());
        }
        catch (...)
        {
        }

        ImGui::Text("Detected DPI Scale: %.2f (%.0f%%)", detected, detected * 100.0f);
        ImGui::SliderFloat("Scale (%)", &dpi_percent, 10.0f, 400.0f, "%.0f%%");
        ImGui::TextWrapped("Apply changes to scale ImGui fonts and widgets. 100% = no scaling.");

        if (ImGui::Button("Apply"))
        {
            // DON'T call applyDPIScale here.
            // Just store the request for the start of the next frame.
            pending_dpi_scale = dpi_percent / 100.0f;

            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
}

void GUIHandler::headerBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New System", "Ctrl+N"))
            {
                fem_system.nodes.clear();
                fem_system.beams.clear();
                fem_system.solve_system();
            }
            if (ImGui::MenuItem("Open...", "Ctrl+O"))
            {
                request_load_popup = true;
            }

            if (ImGui::MenuItem("Save", "Ctrl+S"))
            {
                request_save_popup = true;
            }

            // Examples submenu under File -> Examples
            if (ImGui::BeginMenu("Examples"))
            {
                std::string examples_dir = "resources/examples";
                bool any_ex = false;
                try
                {
                    if (std::filesystem::exists(examples_dir))
                    {
                        for (const auto &entry : std::filesystem::directory_iterator(examples_dir))
                        {
                            if (!entry.is_regular_file())
                                continue;
                            auto p = entry.path();
                            if (p.extension() == ".ffem")
                            {
                                any_ex = true;
                                std::string label = p.filename().string();
                                if (ImGui::MenuItem(label.c_str()))
                                {
                                    std::string full = p.string();
                                    std::strncpy(filename_buf, full.c_str(), sizeof(filename_buf));
                                    filename_buf[sizeof(filename_buf) - 1] = '\0';
                                    trigger_load_read = true;
                                }
                            }
                        }
                    }
                }
                catch (...)
                {
                }

                if (!any_ex)
                    ImGui::TextDisabled("No examples found in resources/examples");

                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                window.close();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Auto View"))
            {
                renderer.centerView();
                renderer.autoZoomToFit();
            }
            if (ImGui::MenuItem("Center View"))
            {
                renderer.centerView();
            }
            if (ImGui::MenuItem("Auto Zoom"))
            {
                renderer.autoZoomToFit();
            }

            if (ImGui::MenuItem("Visualization"))
            {
                show_visualization_editor = !show_visualization_editor;
            }

            if (ImGui::MenuItem("Adjust DPI Scaling"))
            {
                request_dpi_adjust = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("System"))
        {
            if (ImGui::MenuItem("System Controls"))
            {
                show_system_controls = !show_system_controls;
            }
            if (ImGui::MenuItem("System Node Editor"))
            {
                show_node_editor = !show_node_editor;
            }
            if (ImGui::MenuItem("System Beam Editor"))
            {
                show_beam_editor = !show_beam_editor;
            }
            if (ImGui::BeginMenu("Units"))
            {

                if (ImGui::MenuItem("Metric (m, N, Pa)", nullptr, fem_system.unit_system == Metric))
                {
                    fem_system.setUnitSystem(Metric);
                }
                if (ImGui::MenuItem("Imperial (ft, lbf, psi)", nullptr, fem_system.unit_system == ImperialFeet))
                {
                    fem_system.setUnitSystem(ImperialFeet);
                }
                if (ImGui::MenuItem("Imperial (in, lbf, psi)", nullptr, fem_system.unit_system == ImperialInches))
                {
                    fem_system.setUnitSystem(ImperialInches);
                }
                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Materials & Properties"))
        {
            if (ImGui::MenuItem("Material Editor"))
            {
                show_material_editor = !show_material_editor;
            }
            if (ImGui::MenuItem("Profile Editor"))
            {
                show_profile_editor = !show_profile_editor;
            }

            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Output"))
        {
            show_output_tab = !show_output_tab;
        }

        if (ImGui::MenuItem("Help"))
        {
            show_help_page = !show_help_page;
        }

        {
            float frame_rate = ImGui::GetIO().Framerate;
            char fps_buf[32];
            std::snprintf(fps_buf, sizeof(fps_buf), "FPS: %.1f", frame_rate);

            // Compute text width and place it so it never goes past the right window padding
            float text_w = ImGui::CalcTextSize(fps_buf).x;
            ImGuiStyle &style = ImGui::GetStyle();
            float window_w = ImGui::GetWindowWidth();
            float target_x = window_w - text_w - style.WindowPadding.x - style.ItemSpacing.x;

            // Ensure we don't move left of the current cursor position
            float cur_x = ImGui::GetCursorPosX();
            if (target_x < cur_x)
                target_x = cur_x;

            ImGui::SetCursorPosX(target_x);
            ImGui::Text("%s", fps_buf);
        }

        ImGui::EndMainMenuBar();
    }
}

void GUIHandler::helpPage()
{
    if (!show_help_page)
        return;
    ImGui::Begin("Help", &show_help_page, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("FastFEM- Help");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("About", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("This application allows you to create and analyze 2D truss and beam systems using the finite element method. You can add nodes and beams, define material properties and beam profiles, apply forces, and view the resulting displacements and stresses.");
    }

    if (ImGui::CollapsingHeader("How to Use", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("1. Use the 'System Node Editor' to add and manage nodes in your truss system. You can set constraints for each node (fixed, free, or slider).");
        ImGui::TextWrapped("2. Use the 'System Beam Editor' to add beams between nodes. Select material profiles and beam profiles for each beam.");
        ImGui::TextWrapped("3. Use the 'Material Editor' to create and manage material profiles with specific Young's modulus values.");
        ImGui::TextWrapped("4. Use the 'Profile Editor' to create and manage beam profiles with specific cross-sectional areas.");
        ImGui::TextWrapped("5. Use the 'System Controls' to apply forces to nodes and view the computed displacements and stresses.");
        ImGui::TextWrapped("6. Adjust visualization settings in the 'Visualization' window, including displacement scaling and force arrow scaling.");
    }

    if (ImGui::CollapsingHeader("Extra Features", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Extra features as bullet list
        ImGui::BulletText("Save and load truss systems using the 'File' menu. These use the custom .ffem binary format.");
        ImGui::BulletText("Adjust DPI scaling for better visibility on high-resolution displays.");
        ImGui::BulletText("Auto view and zoom features to fit the truss system within the viewport.");
        ImGui::BulletText("Force animation feature to visualize dynamic loading conditions.");
    }

    ImGui::End();
}

namespace
{
    struct FrameTask
    {
        std::string filename;
        sf::Image image; // store the captured image (copied on enqueue)
    };

    static std::deque<FrameTask> g_frame_queue;
    static std::mutex g_frame_mutex;
    static std::condition_variable g_frame_cv;
    static std::atomic<bool> g_writer_running{false};
    static std::atomic<bool> g_writer_started{false};

    static void writerThreadFunc()
    {
        g_writer_running = true;
        while (g_writer_running)
        {
            FrameTask task;
            {
                std::unique_lock<std::mutex> l(g_frame_mutex);
                g_frame_cv.wait(l, []
                                { return !g_frame_queue.empty() || !g_writer_running; });
                if (!g_writer_running && g_frame_queue.empty())
                    break;
                if (g_frame_queue.empty())
                    continue;
                task = std::move(g_frame_queue.front());
                g_frame_queue.pop_front();
            }

            // Save outside lock using sf::Image::saveToFile (no GL context required)
            try
            {
                if (!task.image.saveToFile(task.filename))
                {
                    std::cerr << "Writer thread: failed to save frame " << task.filename << "\n";
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "Writer thread exception: " << e.what() << "\n";
            }
        }
    }
} // namespace

// New: Visualization GUI window
void GUIHandler::visualizationEditor()
{
    if (!show_visualization_editor)
        return;

    ImGui::Begin("Visualization", &show_visualization_editor, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("Rendering / Visualization Settings");
    ImGui::Separator();

    // Show and control displacement scale (public member on renderer)
    float ds = renderer.displacementScale;
    if (ImGui::SliderFloat("Displacement Scale", &ds, 0.0f, 50.0f, "%.2f"))
    {
        renderer.displacementScale = ds;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted("Scale applied to computed displacements for visualization only (does not affect solver).");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // Option: quick reset
    if (ImGui::Button("Reset Scale"))
    {
        renderer.displacementScale = 1.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto Fit View"))
    {
        renderer.centerView();
        renderer.autoZoomToFit();
    }

    // Visual scaling for force/reaction arrows

    float fvs = renderer.forceScale;
    if (ImGui::SliderFloat("Force Visual Scale (N->world)", &fvs, 1.0f, 40000.0f, "%.0f"))
        renderer.forceScale = fvs;

    ImGui::SameLine();
    ImGui::TextDisabled("(larger = shorter arrows)");

    float rvs = renderer.reactionScale;
    if (ImGui::SliderFloat("Reaction Visual Scale (N->world)", &rvs, 1.0f, 40000.0f, "%.0f"))
        renderer.reactionScale = rvs;

    // --- Force animation controls ---
    ImGui::Separator();
    ImGui::Text("Force Animation");
    ImGui::Spacing();

    // Persistent animation state across frames (kept local to this function)
    static bool animate_forces = false;
    static bool prev_animate_forces = false;
    static float animate_time = 0.0f;
    static float animate_speed = 0.5f;     // Hz
    static float animate_amplitude = 1.0f; // multiplier (0 = no change, 1 = ±100%)
    static Eigen::VectorXd saved_forces;
    // New: whether animation applies both positive and negative directions
    static bool bidirectional_forces = true;

    ImGui::Checkbox("Animate Forces", &animate_forces);
    ImGui::SameLine();
    ImGui::Checkbox("Bidirectional Forces", &bidirectional_forces);
    ImGui::SameLine();
    if (ImGui::SmallButton("Capture Current Forces"))
    {
        // Capture baseline forces for the animation
        saved_forces = fem_system.forces;
        animate_time = 0.0f;
    }

    ImGui::SliderFloat("Speed (Hz)", &animate_speed, 0.05f, 5.0f, "%.2f");
    ImGui::SliderFloat("Amplitude (multiplier)", &animate_amplitude, 0.0f, 3.0f, "%.2f");
    ImGui::TextWrapped("Animation scales the captured forces by sin(2*pi*speed*t) * amplitude. Use 'Bidirectional Forces' to clamp to positive-only.");

    // Handle start/stop transitions
    if (animate_forces && !prev_animate_forces)
    {
        // starting animation: ensure we have a baseline
        if (saved_forces.size() != fem_system.forces.size())
            saved_forces = fem_system.forces;
        animate_time = 0.0f;
    }
    else if (!animate_forces && prev_animate_forces)
    {
        // stopped: restore captured forces (if available) and re-solve once
        if (saved_forces.size() == fem_system.forces.size())
        {
            fem_system.forces = saved_forces;
            fem_system.solve_system();
        }
    }
    prev_animate_forces = animate_forces;

    // Perform per-frame animation update when enabled
    if (animate_forces)
    {
        float dt = ImGui::GetIO().DeltaTime; // frame delta
        animate_time += dt;

        float phase = 2.0f * static_cast<float>(M_PI) * animate_speed * animate_time;
        float wave = std::sin(phase);

        float factor;
        if (bidirectional_forces)
        {
            factor = wave * animate_amplitude;
        }
        else
        {
            // Ease in/out: use (sin(phase) + 1) / 2 for smooth 0→1→0
            factor = ((wave + 1.0f) * 0.5f) * animate_amplitude;
        }

        if (saved_forces.size() == fem_system.forces.size() && saved_forces.size() > 0)
        {
            fem_system.forces = saved_forces * factor;
            fem_system.solve_system(); // update deformation for visualization
        }
    }

    // --- Recording controls ---
    ImGui::Separator();
    ImGui::Text("Recording / Export");
    ImGui::Spacing();

    // Persistent state across frames
    static bool recording = false;
    static float record_fps = 24.0;
    static int recorded_frames = 0;
    static double record_accum = 0.0;
    // New: user-configurable recording length (seconds) and elapsed time accumulator
    static float record_length_seconds = 5.0f;
    static double record_time_accum = 0.0;
    static std::string out_dir = "record_frames";
    static std::string out_prefix = "frame";
    static bool auto_build_gif = false;
    static bool delete_frames_after = false;

    // Input fields
    char dir_buf[512];
    std::strncpy(dir_buf, out_dir.c_str(), sizeof(dir_buf));
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    ImGui::InputText("Output Directory", dir_buf, sizeof(dir_buf));
    out_dir = std::string(dir_buf);

    char prefix_buf[128];
    std::strncpy(prefix_buf, out_prefix.c_str(), sizeof(prefix_buf));
    prefix_buf[sizeof(prefix_buf) - 1] = '\0';
    ImGui::InputText("Filename Prefix", prefix_buf, sizeof(prefix_buf));
    out_prefix = std::string(prefix_buf);

    ImGui::SliderFloat("FPS", &record_fps, 1.0, 60.0, "%.0f");
    ImGui::SliderFloat("Length (s)", &record_length_seconds, 0.5f, 600.0f, "%.1f");
    ImGui::Checkbox("Auto-build GIF (requires ImageMagick `convert`)", &auto_build_gif);
    ImGui::Checkbox("Delete frames after GIF created", &delete_frames_after);

    ImGui::Spacing();

    if (!recording)
    {
        if (ImGui::Button("Start Recording"))
        {
            // Prepare output directory
            try
            {
                std::filesystem::create_directories(out_dir);
            }
            catch (...)
            {
                // ignore errors, we'll report failure on save
            }
            recorded_frames = 0;
            record_accum = 0.0;
            record_time_accum = 0.0;
            recording = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Build GIF Now"))
        {
            // Build GIF from existing PNGs in directory using ImageMagick convert
            std::ostringstream cmd;
            // delay is in 1/100s units: delay = 100 / fps
            int delay = static_cast<int>(std::round(100.0 / record_fps));
            std::string pattern = out_dir + "/" + out_prefix + "_*.png";
            std::string out_gif = out_dir + "/" + out_prefix + ".gif";
            cmd << "convert -delay " << delay << " -loop 0 " << pattern << " " << out_gif;
            int rc = std::system(cmd.str().c_str());
            (void)rc; // ignore return for now
        }
    }
    else
    {
        if (ImGui::Button("Stop Recording"))
        {
            recording = false;
        }
    }

    ImGui::SameLine();
    ImGui::Text("Frames: %d", recorded_frames);
    ImGui::SameLine();
    ImGui::Text("Elapsed: %.2fs", record_time_accum);

    // Per-frame capture when recording:
    if (recording)
    {
        // accumulate time and capture at record_fps
        float dt = ImGui::GetIO().DeltaTime;
        record_time_accum += dt;
        record_accum += dt;
        double frame_interval = 1.0 / record_fps;
        while (record_accum >= frame_interval)
        {
            record_accum -= frame_interval;
            // Capture current framebuffer and enqueue for background saving
            try
            {
                sf::Vector2u sz = window.getSize();
                if (sz.x > 0 && sz.y > 0)
                {
                    // Capture framebuffer using OpenGL
                    std::vector<uint8_t> pixels(sz.x * sz.y * 4);
                    glReadPixels(0, 0, sz.x, sz.y, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

                    // Flip vertically (OpenGL origin is bottom-left, image origin is top-left)
                    std::vector<uint8_t> flipped(sz.x * sz.y * 4);
                    for (unsigned int y = 0; y < sz.y; ++y)
                    {
                        std::memcpy(&flipped[y * sz.x * 4],
                                    &pixels[(sz.y - 1 - y) * sz.x * 4],
                                    sz.x * 4);
                    }

                    // Create SFML image from flipped pixel data
                    sf::Image image({sz.x, sz.y}, flipped.data());

                    // Construct filename with zero-padded index
                    std::ostringstream ns;
                    ns << out_dir << "/" << out_prefix << "_"
                       << std::setw(5) << std::setfill('0') << recorded_frames << ".png";
                    std::string fname = ns.str();

                    // Start writer thread lazily
                    if (!g_writer_started.load())
                    {
                        std::thread writer(writerThreadFunc);
                        writer.detach(); // run in background
                        g_writer_started = true;
                    }

                    // Enqueue the sf::Image (copy) for background saving
                    {
                        std::lock_guard<std::mutex> lg(g_frame_mutex);
                        if (g_frame_queue.size() > 500)
                        {
                            g_frame_queue.pop_front();
                        }
                        FrameTask task;
                        task.filename = fname;
                        task.image = std::move(image);
                        g_frame_queue.emplace_back(std::move(task));
                    }
                    g_frame_cv.notify_one();

                    ++recorded_frames;
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "Exception capturing frame: " << e.what() << "\n";
                recording = false;
                break;
            }
        }

        // Auto-stop when elapsed recording time reached (if > 0)
        if (record_length_seconds > 0.0f && record_time_accum >= static_cast<double>(record_length_seconds))
        {
            recording = false;
        }
    }
    else if (!recording && recorded_frames > 0 && auto_build_gif)
    {
        // Wait for queue to drain briefly before building GIF
        const int max_wait_ms = 5000;
        int waited = 0;
        while (waited < max_wait_ms)
        {
            {
                std::lock_guard<std::mutex> lg(g_frame_mutex);
                if (g_frame_queue.empty())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            waited += 50;
        }

        // If queue not empty after waiting, we still attempt to build GIF; user may retry.
        std::ostringstream cmd;
        int delay = static_cast<int>(std::round(100.0 / record_fps));
        std::string pattern = out_dir + "/" + out_prefix + "_*.png";
        std::string out_gif = out_dir + "/" + out_prefix + ".gif";
        cmd << "convert -delay " << delay << " -loop 0 " << pattern << " " << out_gif;
        int rc = std::system(cmd.str().c_str());
        (void)rc;
        if (rc == 0 && delete_frames_after)
        {
            // Remove all matching frames
            try
            {
                for (const auto &p : std::filesystem::directory_iterator(out_dir))
                {
                    std::string name = p.path().filename().string();
                    if (name.rfind(out_prefix + "_", 0) == 0 && p.path().extension() == ".png")
                    {
                        std::filesystem::remove(p.path());
                    }
                }
            }
            catch (...)
            {
            }
            recorded_frames = 0;
        }
        auto_build_gif = false;
    }

    ImGui::End();
}
