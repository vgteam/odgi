#include "path_sgd.hpp"

#define debug_path_sgd
namespace odgi {
    namespace algorithms {

        std::vector<double> path_linear_sgd(const PathHandleGraph &graph,
                                            const xp::XP &path_index,
                                            const std::set<std::string> path_sgd_use_paths,
                                            const uint64_t &iter_max,
                                            const uint64_t &min_term_updates,
                                            const double &delta,
                                            const double &eps,
                                            const double &theta,
                                            const uint64_t &space,
                                            const uint64_t &nthreads) {
            using namespace std::chrono_literals; // for timing stuff
            // our positions in 1D
            std::vector<std::atomic<double>> X(graph.get_node_count());
            // seed them with the graph order
            uint64_t len = 0;
            graph.for_each_handle(
                    [&X, &graph, &len](const handle_t &handle) {
                        // nb: we assume that the graph provides a compact handle set
                        X[number_bool_packing::unpack_number(handle)].store(len);
                        len += graph.get_length(handle);
                    });
            // the longest path length measured in nucleotides
            size_t longest_path_in_nucleotides = 0;
            // the total path length in nucleotides
            size_t total_path_len_in_nucleotides = 0;
            // here we store all path nucleotides lengths so we know later from which path we sampled our random position from
            IntervalTree<size_t, path_handle_t> path_nucleotide_tree;
            std::vector<Interval<size_t, path_handle_t> > path_nucleotide_intervals;
            // iterate over all relevant path_handles:
            //  1. build the interval tree
            //  2. find out the longest path in nucleotides and store this number size_t
            //  3. add the current path length to the total length
            for (auto path_name : path_sgd_use_paths) {
                path_handle_t path = path_index.get_path_handle(path_name);
#ifdef debug_path_sgd
                std::cerr << path_name << std::endl;
                std::cerr << as_integer(path) << std::endl;
#endif
                size_t path_len = path_index.get_path_length(path);
#ifdef debug_path_sgd
                std::cerr << path_name << " has length: " << path_len << std::endl;
#endif
                path_nucleotide_intervals.push_back(Interval<size_t, path_handle_t>(total_path_len_in_nucleotides + 1,
                                                                                    total_path_len_in_nucleotides +
                                                                                    path_len, path));
                if (path_len > longest_path_in_nucleotides) {
                    longest_path_in_nucleotides = path_len;
                }
                total_path_len_in_nucleotides += path_len;
            }
            path_nucleotide_tree = IntervalTree<size_t, path_handle_t>(
                    static_cast<IntervalTree<unsigned long, path_handle_t>::interval_vector &&>(path_nucleotide_intervals));

#ifdef debug_path_sgd

            std::vector<Interval<size_t, path_handle_t> > results;
            results = path_nucleotide_tree.findOverlapping(2, 2);
            std::cerr << "found " << results.size() << " overlapping intervals" << std::endl;
            std::cerr << "found " << results[0].start << " start in intervals" << std::endl;
            std::cerr << "found " << results[0].stop << " stop in intervals" << std::endl;
            std::cerr << "the real path position " << 2 - results[0].start + 1 << std::endl;
            std::cerr << "found " << path_index.get_path_name(results[0].value) << " value in intervals" << std::endl;
            results = path_nucleotide_tree.findOverlapping(20, 20);
            std::cerr << "found " << results.size() << " overlapping intervals" << std::endl;
            std::cerr << "found " << results[0].start << " start in intervals" << std::endl;
            std::cerr << "found " << results[0].stop << " stop in intervals" << std::endl;
            std::cerr << "found " << path_index.get_path_name(results[0].value) << " value in intervals" << std::endl;
            std::cerr << "the real path position is: " << 20 - results[0].start + 1 << std::endl;
#endif

            double w_min = 1 / (longest_path_in_nucleotides * longest_path_in_nucleotides);
            double w_max = 1;
            // get our schedule
            std::vector<double> etas = path_linear_sgd_schedule(w_min, w_max, iter_max, eps);
            // initialize Zipfian distrubution so we only have to calculate zeta once
            std::default_random_engine generator;
            zipfian_int_distribution<int>::param_type p(1, space, theta);
            zipfian_int_distribution<int> distribution(p);
            // how many term updates we make
            std::atomic<uint64_t> term_updates;
            term_updates.store(0);
            // learning rate
            std::atomic<double> eta;
            eta.store(etas.front());
            // our max delta
            std::atomic<double> Delta_max;
            Delta_max.store(0);
            // should we keep working?
            std::atomic<bool> work_todo;
            work_todo.store(true);
            // approximately what iteration we're on
            uint64_t iteration = 0;
            // launch a thread to update the learning rate, count iterations, and decide when to stop
            auto checker_lambda =
                    [&](void) {
                        while (work_todo.load()) {
                            if (term_updates.load() > min_term_updates) {
                                if (++iteration > iter_max) {
                                    work_todo.store(false);
                                } else if (Delta_max.load() <= delta) { // nb: this will also break at 0
                                    work_todo.store(false);
                                } else {
//#pragma omp critical (cerr)
                                    /*
                                    std::cerr << iteration
                                              << ", eta: " << eta.load()
                                              << ", Delta: " << Delta_max.load()
                                              << " terms " << terms.size()
                                              << " updates " << term_updates.load() << std::endl;
                                    */
                                    eta.store(etas[iteration]); // update our learning rate
                                    Delta_max.store(delta); // set our delta max to the threshold
                                }
                                term_updates.store(0);
                            }
                            std::this_thread::sleep_for(1ms);
                        }
                    };
// TODO continue here
            auto worker_lambda =
                    [&](uint64_t tid) {
                        // everyone tries to seed with their own random data
                        std::random_device rd;
                        std::mt19937 gen(rd());
                        std::uniform_int_distribution<uint64_t> dis(1, total_path_len_in_nucleotides);
                        while (work_todo.load()) {
                            // TODO pack this into its own function
                            // pick a random position from all paths
                            uint64_t pos = dis(gen);
                            // use our interval tree to get the path handle and path nucleotide position of the picked position
                            std::vector<Interval<size_t, path_handle_t> > result;
                            result = path_nucleotide_tree.findOverlapping(pos, pos);
                            path_handle_t path = result[0].value;
                            size_t path_start_pos = result[0].start;
                            size_t path_end_pos = result[0].stop;
                            // we have a 0-based positioning in the path index
                            size_t pos_in_path = pos - path_start_pos;
                            std::string path_name = path_index.get_path_name(path);
                            size_t pangenome_pos_uniform = path_index.get_pangenome_pos(path_name, pos_in_path);
                            int zipf_int = distribution(generator);
                            if (path_end_pos < zipf_int) {
                                zipf_int = zipf_int % (zipf_int - pos_in_path);
                            }
                            size_t pangenome_pos_zipf = path_index.get_pangenome_pos(path_name, zipf_int);
                            size_t term_dist = abs(static_cast<long long>(pangenome_pos_uniform - pangenome_pos_zipf));
                            double term_weight = 1 / term_dist;
                            handle_t term_i = as_handle(pangenome_pos_uniform);
                            handle_t term_j = as_handle(pangenome_pos_zipf);
                            sgd_term_t t = sgd_term_t(term_i, term_j, term_dist, term_weight);

                            double w_ij = t.w;
                            double mu = eta.load() * w_ij;
                            if (mu > 1) {
                                mu = 1;
                            }
                            // actual distance in graph
                            double d_ij = t.d;
                            // identities
                            uint64_t i = number_bool_packing::unpack_number(t.i);
                            uint64_t j = number_bool_packing::unpack_number(t.j);
//#pragma omp critical (cerr)
//                  std::cerr << "nodes are " << graph.get_id(t.i) << " and " << graph.get_id(t.j) << std::endl;
                            // distance == magnitude in our 1D situation
                            double dx = X[i].load() - X[j].load();
                            if (dx == 0) {
                                dx = 1e-9; // avoid nan
                            }
//#pragma omp critical (cerr)
//                    std::cerr << "distance is " << dx << " but should be " << d_ij << std::endl;
                            //double mag = dx; //sqrt(dx*dx + dy*dy);
                            double mag = sqrt(dx * dx);
                            // check distances for early stopping
                            double Delta = mu * (mag - d_ij) / 2;
                            // try until we succeed. risky.
                            double Delta_abs = std::abs(Delta);
//#pragma omp critical (cerr)
//                    std::cerr << "Delta_abs " << Delta_abs << std::endl;
                            while (Delta_abs > Delta_max.load()) {
                                Delta_max.store(Delta_abs);
                            }
                            // calculate update
                            double r = Delta / mag;
                            double r_x = r * dx;
//#pragma omp critical (cerr)
//                    std::cerr << "r_x is " << r_x << std::endl;
                            // update our positions (atomically)
                            X[i].store(X[i].load() - r_x);
                            X[j].store(X[j].load() + r_x);
                            term_updates.store(term_updates.load() + 1);
                        }
                    };

            std::thread checker(checker_lambda);

            std::vector<std::thread> workers;
            workers.reserve(nthreads);
            for (uint64_t t = 0; t < nthreads; ++t) {
                workers.emplace_back(worker_lambda, t);
            }

            for (uint64_t t = 0; t < nthreads; ++t) {
                workers[t].join();
            }
            checker.join();
            // drop out of atomic stuff... maybe not the best way to do this
            std::vector<double> X_final(X.size());
            uint64_t i = 0;
            for (auto &x : X) {
                X_final[i++] = x.load();
            }
            return X_final;
        }

        std::vector<double> path_linear_sgd_schedule(const double &w_min,
                                                     const double &w_max,
                                                     const uint64_t &iter_max,
                                                     const double &eps) {
            double eta_max = 1.0 / w_min;
            double eta_min = eps / w_max;
            double lambda = log(eta_max / eta_min) / ((double) iter_max - 1);
            // initialize step sizes
            std::vector<double> etas;
            etas.reserve(iter_max);
            for (uint64_t t = 0; t < iter_max; t++) {
                etas.push_back(eta_max * exp(-lambda * t));
            }
            return etas;
        }

        std::vector<handle_t> path_linear_sgd_order(const PathHandleGraph &graph,
                                                    const xp::XP &path_index,
                                                    const std::set<std::string> path_sgd_use_paths,
                                                    const uint64_t &iter_max,
                                                    const uint64_t &min_term_updates,
                                                    const double &delta,
                                                    const double &eps,
                                                    const double &theta,
                                                    const uint64_t &space,
                                                    const uint64_t &nthreads) {
            std::vector<double> layout = path_linear_sgd(graph,
                                                         path_index,
                                                         path_sgd_use_paths,
                                                         iter_max,
                                                         min_term_updates,
                                                         delta,
                                                         eps,
                                                         theta,
                                                         space,
                                                         nthreads);
            std::vector<std::pair<double, handle_t>> layout_handles;
            uint64_t i = 0;
            graph.for_each_handle([&i, &layout, &layout_handles](const handle_t &handle) {
                layout_handles.push_back(
                        std::make_pair(
                                layout[i++],
                                handle));
            });
            std::sort(layout_handles.begin(), layout_handles.end(),
                      [&](const std::pair<double, handle_t> &a,
                          const std::pair<double, handle_t> &b) {
                          return a.first < b.first
                                 || (a.first == b.first
                                     && as_integer(a.second) < as_integer(b.second));
                      });
            std::vector<handle_t> order;
            for (auto &p : layout_handles) {
                order.push_back(p.second);
            }
            return order;
        }

    }
}