#ifndef OPENFINRAM_OPENROAD_TCL_GENERATOR_HPP_
#define OPENFINRAM_OPENROAD_TCL_GENERATOR_HPP_

#include <string>
#include <vector>
#include <map>

namespace OpenFinRAM {

struct PinInfo2 {
    std::string name;
    double x;
};

struct QoRReport2 {
    double cell_area = 0.0;
    bool valid = false;
};

class OpenRoadTclGenerator {
public:
    OpenRoadTclGenerator();
    ~OpenRoadTclGenerator() = default;

    void set_design_name(const std::string& n) { design_name_ = n; }
    void set_site_name(const std::string& n) { site_name_ = n; }
    void set_site_height(double height);
    void set_bitcell_width(double width);
    void set_cpu_count(int local_cpu, int remote_cpu = 0);

    bool parse_qor_report(const std::string& qor_file);
    const QoRReport2& get_qor_report() const { return qor_; }

    double calculate_floorplan_height(double width) const;
    double align_to_site_height(double h) const;
    std::string generate_floorplan_command(double& w, double& h) const;

    // main TCL for OpenROAD flow (replaces Innovus run.tcl)
    bool generate_run_tcl(double width, double height,
                          const std::string& output_file,
                          int num_wlt, int num_wlb, int num_ysel,
                          int addr_width, int num_mux,
                          bool spice_only, double col_width,
                          const std::string& platform_path,
                          const std::string& tech_root) const;

    bool run_openroad(const std::string& tcl_file,
                      const std::string& work_dir,
                      const std::string& log_file = "openroad.log",
                      const std::string& openroad_bin = "openroad") const;

    bool stream_def_to_gds(const std::string& def_file,
                           const std::string& tech_lef,
                           const std::string& cell_lef,
                           const std::string& macro_gds,
                           const std::string& output_gds,
                           const std::string& script_path = "") const;

    bool run_v2lvs(const std::string& work_dir,
                   const std::string& verilog_file = "netlist_for_lvs.v",
                   const std::string& spice_file = "netlist_for_lvs.sp",
                   const std::string& cdl_file = "") const;

    bool post_process_netlist(const std::string& work_dir,
                              const std::string& spice_file,
                              const std::string& cdl_file);

private:
    std::string design_name_ = "ctrl_decode";
    std::string site_name_ = "asap7sc7p5t";
    double site_height_ = 0.27;
    double bitcell_width_ = 0.108;
    int local_cpu_ = 8;
    QoRReport2 qor_;

    // helpers
    bool file_exists(const std::string& p) const;
    std::vector<std::string> read_file(const std::string& p) const;
    bool write_file(const std::string& p, const std::vector<std::string>& lines) const;
    std::vector<std::string> merge_continuation(const std::vector<std::string>& lines) const;
    std::vector<std::string> add_power_to_subckt(const std::vector<std::string>& lines) const;
    bool parse_cdl(const std::string& cdl_path);
    void parse_subckt_from_lines(const std::vector<std::string>& lines);
    std::vector<std::string> expand_pins(const std::vector<std::string>& lines);
    std::vector<std::string> process_connect(const std::vector<std::string>& lines) const;

    std::map<std::string, std::vector<std::string>> subckt_dict_;
};

} // namespace OpenFinRAM

#endif // OPENFINRAM_OPENROAD_TCL_GENERATOR_HPP_
