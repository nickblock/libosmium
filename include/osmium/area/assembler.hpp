#ifndef OSMIUM_AREA_ASSEMBLER_HPP
#define OSMIUM_AREA_ASSEMBLER_HPP

/*

This file is part of Osmium (http://osmcode.org/osmium).

Copyright 2013,2014 Jochen Topf <jochen@topf.org> and others (see README).

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#include <algorithm>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <vector>

#include <osmium/memory/buffer.hpp>
#include <osmium/osm/area.hpp>
#include <osmium/osm/builder.hpp>
#include <osmium/osm/location.hpp>
#include <osmium/osm/ostream.hpp>
#include <osmium/osm/relation.hpp>

#include <osmium/area/segment.hpp>
#include <osmium/area/problem.hpp>
#include <osmium/area/detail/proto_ring.hpp>

namespace osmium {

    namespace area {

        using osmium::area::detail::ProtoRing;

        namespace detail {

            inline bool is_below(const osmium::Location& loc, const NodeRefSegment& seg) {
                double ax = seg.first().location().x();
                double bx = seg.second().location().x();
                double cx = loc.x();
                double ay = seg.first().location().y();
                double by = seg.second().location().y();
                double cy = loc.y();
                return ((bx - ax)*(cy - ay) - (by - ay)*(cx - ax)) <= 0;
            }

        }

        /**
         * Assembles area objects from multipolygon relations and their
         * members. This is called by the Collector object after all
         * members have been collected.
         */
        class Assembler {

            // List of problems found when assembling areas
            std::vector<Problem> m_problems {};

            // Enables list of problems to be kept
            bool m_remember_problems { false };

            // Enables debug output to stderr
            bool m_debug { false };

            // The way segments
            std::vector<NodeRefSegment> m_segments;

            // The rings we are building from the way segments
            std::list<ProtoRing> m_rings;

            /**
             * Extract all segments from all ways that make up this
             * multipolygon relation. The segments all have their smaller
             * coordinate at the beginning of the segment. Smaller, in this
             * case, means smaller x coordinate, and if they are the same
             * smaller y coordinate.
             */
            void extract_segments_from_ways(const std::vector<size_t>& members, const osmium::memory::Buffer& in_buffer) {
                for (size_t offset : members) {
                    const osmium::Way& way = in_buffer.get<const osmium::Way>(offset);
                    osmium::NodeRef last_nr;
                    for (osmium::NodeRef nr : way.nodes()) {
                        if (last_nr.location() && last_nr != nr) {
                            m_segments.push_back(NodeRefSegment(last_nr, nr));
                        }
                        last_nr = nr;
                    }
                }
            }

            /**
             * Find duplicate segments (ie same start and end point) and
             * remove them. This will always remove pairs of the same
             * segment. So if there are three, for instance, two will be
             * removed and one will be left.
             */
            void find_and_erase_duplicate_segments() {
                while (true) {
                    std::vector<NodeRefSegment>::iterator found = std::adjacent_find(m_segments.begin(), m_segments.end());
                    if (found == m_segments.end()) {
                        break;
                    }
                    if (m_debug) {
                        std::cerr << "  erase duplicate segment: " << *found << "\n";
                    }
                    m_segments.erase(found, found+2);
                }
            }

            /**
             * Find intersection between segments.
             *
             * @returns true if there are intersections.
             */
            bool find_intersections() {
                if (m_segments.begin() == m_segments.end()) {
                    return false;
                }

                bool found_intersections = false;

                for (auto it1 = m_segments.begin(); it1 != m_segments.end()-1; ++it1) {
                    const NodeRefSegment& s1 = *it1;
                    for (auto it2 = it1+1; it2 != m_segments.end(); ++it2) {
                        const NodeRefSegment& s2 = *it2;
                        if (s1 == s2) {
                            if (m_debug) {
                                std::cerr << "  found overlap on segment " << s1 << "\n";
                            }
                        } else {
                            if (outside_x_range(s2, s1)) {
                                break;
                            }
                            if (y_range_overlap(s1, s2)) {
                                osmium::Location intersection = calculate_intersection(s1, s2);
                                if (intersection) {
                                    found_intersections = true;
                                    if (m_debug) {
                                        std::cerr << "  segments " << s1 << " and " << s2 << " intersecting at " << intersection << "\n";
                                    }
                                    if (m_remember_problems) {
                                        m_problems.emplace_back(Problem(osmium::area::Problem::problem_type::intersection, osmium::NodeRef(0, intersection), s1, s2));
                                    }
                                }
                            }
                        }
                    }
                }

                return found_intersections;
            }

            /**
             * Initialize area attributes and tags from the attributes and tags
             * of the relation.
             */
            void initialize_area_from_relation(osmium::osm::AreaBuilder& builder, const osmium::Relation& relation) const {
                osmium::Area& area = builder.object();
                area.id(relation.id() * 2 + 1);
                area.version(relation.version());
                area.changeset(relation.changeset());
                area.timestamp(relation.timestamp());
                area.visible(relation.visible());
                area.uid(relation.uid());

                builder.add_user(relation.user());

                osmium::osm::TagListBuilder tl_builder(builder.buffer(), &builder);
                for (const osmium::Tag& tag : relation.tags()) {
                    tl_builder.add_tag(tag.key(), tag.value());
                }
            }

            /**
             * Segments have a pointer to the ring they are in. If two rings
             * are merged, all segments need to be updated that point to the
             * ring that is merged into the other. This function does that.
             */
            void update_ring_link_in_segments(const ProtoRing* old_ring, ProtoRing* new_ring) {
                for (NodeRefSegment& segment : m_segments) {
                    if (segment.ring() == old_ring) {
                        segment.ring(new_ring);
                    }
                }
            }

            /**
             * Go through all the rings and find rings that are not closed.
             * Problem objects are created for the end points of the open
             * rings and placed into the m_problems collection.
             *
             * @returns true if any rings were not closed, false otherwise
             */
            bool check_for_open_rings() {
                bool open_rings = false;

                for (auto& ring : m_rings) {
                    if (!ring.closed()) {
                        open_rings = true;
                        if (m_remember_problems) {
                            m_problems.emplace_back(Problem(osmium::area::Problem::problem_type::ring_not_closed, ring.first()));
                            m_problems.emplace_back(Problem(osmium::area::Problem::problem_type::ring_not_closed, ring.last()));
                        }
                    }
                }

                return open_rings;
            }

            /**
             * Check whether there are any rings that can be combined with the
             * given ring to one larger ring by appending the other ring to
             * the end of this ring.
             * If the rings can be combined they are and the function returns
             * a pointer to the old ring that is not used any more.
             */
            ProtoRing* possibly_combine_rings_end(ProtoRing& ring) {
                osmium::Location location = ring.last().location();

                if (m_debug) {
                    std::cerr << "      combine_rings_end\n";
                }
                for (auto it = m_rings.begin(); it != m_rings.end(); ++it) {
                    if (&*it != &ring && !it->closed()) {
                        if ((location == it->first().location())) {
                            ring.merge_ring(*it, m_debug);
                            ProtoRing* ring_ptr = &*it;
                            m_rings.erase(it);
                            return ring_ptr;
                        }
                    }
                }
                return nullptr;
            }

            /**
             * Check whether there are any rings that can be combined with the
             * given ring to one larger ring by prepending the other ring to
             * the start of this ring.
             * If the rings can be combined they are and the function returns
             * a pointer to the old ring that is not used any more.
             */
            ProtoRing* possibly_combine_rings_start(ProtoRing& ring) {
                osmium::Location location = ring.first().location();

                if (m_debug) {
                    std::cerr << "      combine_rings_start\n";
                }
                for (auto it = m_rings.begin(); it != m_rings.end(); ++it) {
                    if (&*it != &ring && !it->closed()) {
                        if ((location == it->last().location())) {
                            ring.swap_nodes(*it);
                            ring.merge_ring(*it, m_debug);
                            ProtoRing* ring_ptr = &*it;
                            m_rings.erase(it);
                            return ring_ptr;
                        }
                    }
                }
                return nullptr;
            }

            bool has_closed_subring_end(ProtoRing& ring, const NodeRef& node_ref) {
                if (m_debug) {
                    std::cerr << "      has_closed_subring_end()\n";
                }
                osmium::Location loc = node_ref.location();
                if (loc == ring.first().location()) {
                    if (m_debug) {
                        std::cerr << "        ring now closed\n";
                        return true;
                    }
                } else {
                    for (auto it = ring.nodes().begin(); it != ring.nodes().end() - 1; ++it) {
                        if (it->location() == loc) {
                            if (m_debug) {
                                std::cerr << "        subring found at: " << *it << "\n";
                            }
                            ProtoRing new_ring(it, ring.nodes().end());
                            ring.remove_nodes(it+1, ring.nodes().end());
                            if (m_debug) {
                                std::cerr << "        split into two rings:\n";
                                std::cerr << "          " << new_ring << "\n";
                                std::cerr << "          " << ring << "\n";
                            }
                            m_rings.push_back(new_ring);
                            return true;
                        }
                    }
                }
                return false;
            }

            bool has_closed_subring_start(ProtoRing& ring, const NodeRef& node_ref) {
                if (m_debug) {
                    std::cerr << "      has_closed_subring_start()\n";
                }
                osmium::Location loc = node_ref.location();
                if (loc == ring.last().location()) {
                    if (m_debug) {
                        std::cerr << "        ring now closed\n";
                        return true;
                    }
                } else {
                    for (auto it = ring.nodes().begin() + 1; it != ring.nodes().end(); ++it) {
                        if (it->location() == loc) {
                            if (m_debug) {
                                std::cerr << "        subring found at: " << *it << "\n";
                            }
                            ProtoRing new_ring(ring.nodes().begin(), it+1);
                            ring.remove_nodes(ring.nodes().begin(), it);
                            if (m_debug) {
                                std::cerr << "        split into two rings:\n";
                                std::cerr << "          " << new_ring << "\n";
                                std::cerr << "          " << ring << "\n";
                            }
                            m_rings.push_back(new_ring);
                            return true;
                        }
                    }
                }
                return false;
            }

            void combine_rings(NodeRefSegment& segment, const NodeRef& node_ref, ProtoRing& ring, bool at_end) {
                if (m_debug) {
                    std::cerr << "      match\n";
                }
                segment.ring(&ring);

                ProtoRing* pr = nullptr;
                if (at_end) {
                    ring.add_location_end(node_ref);
                    if (has_closed_subring_end(ring, node_ref)) {
//                        return;
                    }
                    pr = possibly_combine_rings_end(ring);
                } else {
                    ring.add_location_start(node_ref);
                    if (has_closed_subring_start(ring, node_ref)) {
//                        return;
                    }
                    pr = possibly_combine_rings_start(ring);
                }

                if (pr) {
                    update_ring_link_in_segments(pr, &ring);
                }
            }

            /**
             * Append each outer ring together with its inner rings to the
             * area in the buffer.
             */
            void add_rings_to_area(osmium::osm::AreaBuilder& builder, const std::vector<const ProtoRing*> outer_rings) const {
                for (const ProtoRing* ring : outer_rings) {
                    if (m_debug) {
                        std::cerr << "    ring " << *ring << " is outer\n";
                    }
                    osmium::osm::OuterRingBuilder ring_builder(builder.buffer(), &builder);
                    for (auto& node_ref : ring->nodes()) {
                        ring_builder.add_node_ref(node_ref);
                    }
                    for (ProtoRing* inner : ring->inner_rings()) {
                        osmium::osm::InnerRingBuilder ring_builder(builder.buffer(), &builder);
                        for (auto& node_ref : inner->nodes()) {
                            ring_builder.add_node_ref(node_ref);
                        }
                    }
                    builder.buffer().commit();
                }
            }

        public:

            Assembler() = default;

            ~Assembler() = default;

            /**
             * Enable or disable debug output to stderr. This is for Osmium
             * developers only.
             */
            void enable_debug_output(bool debug=true) {
                m_debug = debug;
            }

            /**
             * Enable or disable collection of problems in the input data.
             * If this is enabled the assembler will keep a list of all
             * problems found (such as self-intersections and unclosed rings).
             * This creates some overhead so it is disabled by default.
             */
            void remember_problems(bool remember=true) {
                m_remember_problems = remember;
            }

            /**
             * Clear the list of problems that have been found.
             */
            void clear_problems() {
                m_problems.clear();
            }

            /**
             * Get the list of problems found so far in the input data.
             */
            const std::vector<Problem>& problems() const {
                return m_problems;
            }

            /**
             * Assemble an area from the given relation and its members.
             * All members are to be found in the in_buffer at the offsets
             * given by the members parameter.
             * The resulting area is put into the out_buffer.
             */
            void operator()(const osmium::Relation& relation, const std::vector<size_t>& members, const osmium::memory::Buffer& in_buffer, osmium::memory::Buffer& out_buffer) {
                m_segments.clear();
                m_rings.clear();

                extract_segments_from_ways(members, in_buffer);

                if (m_debug) {
                    std::cerr << "\nBuild relation id()=" << relation.id() << " members.size()=" << members.size() << " segments.size()=" << m_segments.size() << "\n";
                }

                // Now all of these segments will be sorted. Again, smaller, in
                // this case, means smaller x coordinate, and if they are the
                // same smaller y coordinate.
                std::sort(m_segments.begin(), m_segments.end());

                // remove empty segments
/*                m_segments.erase(std::remove_if(m_segments.begin(), m_segments.end(), [](osmium::UndirectedSegment& segment) {
                    return segment.first() == segment.second();
                }));*/

                find_and_erase_duplicate_segments();

                // Now create the Area object and add the attributes and tags
                // from the relation.
                osmium::osm::AreaBuilder builder(out_buffer);
                initialize_area_from_relation(builder, relation);

                // From now on we have an area object without any rings in it.
                // Areas without rings are "defined" to be invalid. We commit
                // this area and the caller of the assembler will see the
                // invalid area. If all goes well, we later add the rings, commit
                // again, and thus make a valid area out of it.
                out_buffer.commit();

                // Now we look for segments crossing each other. If there are
                // any, the multipolygon is invalid.
                // In the future this could be improved by trying to fix those
                // cases.
                if (find_intersections()) {
                    return;
                }

                // Now iterator over all segments and add them to rings
                // until there are no segments left.
                for (auto it = m_segments.begin(); it != m_segments.end(); ++it) {
                    auto& segment = *it;

                    if (m_debug) {
                        std::cerr << "  check segment " << segment << "\n";
                    }

                    int n=0;
                    for (auto& ring : m_rings) {
                        if (m_debug) {
                            std::cerr << "    check against ring " << n << " " << ring << "\n";
                        }
                        if (!ring.closed()) {
                            if (ring.last() == segment.first() ) {
                                combine_rings(segment, segment.second(), ring, true);
                                goto next_segment;
                            }
                            if (ring.last() == segment.second() ) {
                                combine_rings(segment, segment.first(), ring, true);
                                goto next_segment;
                            }
                            if (ring.first() == segment.first() ) {
                                combine_rings(segment, segment.second(), ring, false);
                                goto next_segment;
                            }
                            if (ring.first() == segment.second() ) {
                                combine_rings(segment, segment.first(), ring, false);
                                goto next_segment;
                            }
                        } else {
                            if (m_debug) {
                                std::cerr << "      ring CLOSED\n";
                            }
                        }

                        ++n;
                    }

                    {
                        if (m_debug) {
                            std::cerr << "    new ring for segment " << segment << "\n";
                        }

                        bool cw = true;

                        if (it != m_segments.begin()) {
                            osmium::Location loc = segment.first().location();
                            if (m_debug) {
                                std::cerr << "      compare against id=" << segment.first().ref() << " lat()=" << loc.lat() << "\n";
                            }
                            for (auto oit = it-1; oit != m_segments.begin()-1; --oit) {
                                if (m_debug) {
                                    std::cerr << "      seg=" << *oit << "\n";
                                }
                                std::pair<int32_t, int32_t> mm = std::minmax(oit->first().location().y(), oit->second().location().y());
                                if (mm.first <= loc.y() && mm.second >= loc.y()) {
                                    if (m_debug) {
                                        std::cerr << "        in range\n";
                                    }
                                    if (oit->first().location().x() < loc.x() &&
                                        oit->second().location().x() < loc.x()) {
                                        std::cerr << "          if 1\n";
                                        cw = !oit->cw();
                                        segment.left_segment(&*oit);
                                        break;
                                    }
                                    if (detail::is_below(loc, *oit)) { // XXX
                                        std::cerr << "          if 2\n";
                                        cw = !oit->cw();
                                        segment.left_segment(&*oit);
                                        break;
                                    }
                                    std::cerr << "          else\n";
                                }
                            }
                        }

                        if (m_debug) {
                            std::cerr << "      is " << (cw ? "cw" : "ccw") << "\n";
                        }

                        segment.cw(cw);
                        m_rings.emplace_back(ProtoRing(segment));
                        segment.ring(&m_rings.back());
                    }

                    next_segment:
                        ;

                }

                if (m_debug) {
                    std::cerr << "  Rings:\n";
                    for (auto& ring : m_rings) {
                        std::cerr << "    " << ring;
                        if (ring.closed()) {
                            std::cerr << " (closed)";
                        }
                        std::cerr << "\n";
                    }
                }

                if (check_for_open_rings()) {
                    if (m_debug) {
                        std::cerr << "  not all rings are closed\n";
                    }
                    return;
                }

                if (m_debug) {
                    std::cerr << "  Find inner/outer...\n";
                }

                // Find inner rings to each outer ring.
                std::vector<const ProtoRing*> outer_rings;
                for (auto& ring : m_rings) {
                    if (ring.is_outer()) {
                        if (m_debug) {
                            std::cerr << "    Outer: " << ring << "\n";
                        }
                        outer_rings.push_back(&ring);
                    } else {
                        if (m_debug) {
                            std::cerr << "    Inner: " << ring << "\n";
                        }
                        ProtoRing* outer = ring.find_outer(m_segments, m_debug);
                        if (outer) {
                            outer->add_inner_ring(&ring);
                        } else {
                            if (m_debug) {
                                std::cerr << "    something bad happened\n";
                            }
                            return;
                        }
                    }
                }

                add_rings_to_area(builder, outer_rings);
            }

        }; // class Assembler

    } // namespace area

} // namespace osmium

#endif // OSMIUM_AREA_ASSEMBLER_HPP
