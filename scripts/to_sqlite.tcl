#!/usr/bin/env tclsh
package require Tcl 8.6
package require sqlite3
namespace import ::tcl::mathop::*

proc convert_sel_list {ch elem_ids db sql} {
	binary scan [read $ch 4] iu num_sel
	for {set i 0} {$i < $num_sel} {incr i} {
		binary scan [read $ch 4] iu elem
		set id [lindex $elem_ids $elem]
		$db eval $sql
	}
}

proc read_string {ch} {
	binary scan [read $ch 2] su str_len
	return [read $ch $str_len]
}

proc convert_winged_file {ch db} {
	if {[read $ch 4] != "GNIW"} {
		error "Unrecognized file format"
	}
	binary scan [read $ch 4] iu version
	if {$version != 2} {
		error "Unrecognized file version: $version"
	}

	$db config trusted_schema 0
	$db config defensive 1
	$db config dqs_dml 0
	$db config dqs_ddl 0
	$db config enable_fkey 1
	$db bind_fallback error

	$db eval {
		BEGIN TRANSACTION;

		CREATE TABLE library(rowid INTEGER PRIMARY KEY, path TEXT UNIQUE);
		CREATE TABLE paints(
			rowid INTEGER PRIMARY KEY,
			material INTEGER REFERENCES library,
			axes_00 INTEGER, axes_01 INTEGER,
			axes_10 INTEGER, axes_11 INTEGER,
			axes_20 INTEGER, axes_21 INTEGER,
			axes_30 INTEGER, axes_31 INTEGER,
			tf_00 INTEGER, tf_01 INTEGER,
			tf_10 INTEGER, tf_11 INTEGER,
			tf_20 INTEGER, tf_21 INTEGER
		);
		CREATE TABLE verts(rowid INTEGER PRIMARY KEY, x REAL, y REAL, z REAL);
		CREATE TABLE faces(rowid INTEGER PRIMARY KEY, paint INTEGER REFERENCES paints);
		CREATE TABLE edges(
			rowid INTEGER PRIMARY KEY,
			twin INTEGER REFERENCES edges UNIQUE,
			vert INTEGER REFERENCES verts,
			face INTEGER REFERENCES faces,
			UNIQUE(vert, face)
		);
		CREATE TABLE sel_verts(vert INTEGER REFERENCES verts);
		CREATE TABLE sel_faces(face INTEGER REFERENCES faces);
		CREATE TABLE sel_edges(edge INTEGER REFERENCES edges);
		CREATE TABLE editor(
			sel_mode INTEGER,
			grid_on INTEGER,
			grid_size REAL,
			work_org_x REAL, work_org_y REAL, work_org_z REAL,
			work_norm_x REAL, work_norm_y REAL, work_norm_z REAL
		);
		CREATE TABLE views(
			cam_x REAL, cam_y REAL, cam_z REAL,
			rot_x REAL, rot_y REAL, zoom REAL,
			view_mode INTEGER, pick_type INTEGER
		);

		PRAGMA application_id = 0x57494e47;
		PRAGMA user_version = 3;
	}

	binary scan [read $ch 4] iu num_paints
	binary scan [read $ch 4] iu num_faces
	binary scan [read $ch 4] iu num_verts
	binary scan [read $ch 4] iu num_edges

	set paint_ids [list]
	set paint_mat_uuids [list]
	for {set p 0} {$p < $num_paints} {incr p} {
		set mat_uuid [read $ch 16]
		binary scan [read $ch [* 4 2 4]] f* tex_axes
		binary scan [read $ch [* 3 2 4]] f* tex_tf
		$db eval "
			INSERT INTO paints(
			    axes_00, axes_01, axes_10, axes_11, axes_20, axes_21, axes_30, axes_31,
				tf_00, tf_01, tf_10, tf_11, tf_20, tf_21
			) VALUES([join $tex_axes {,}], [join $tex_tf {,}])
		"
		lappend paint_ids [$db last_insert_rowid]
		lappend paint_mat_uuids $mat_uuid
	}

	set face_ids [list]
	for {set f 0} {$f < $num_faces} {incr f} {
		binary scan [read $ch 4] iu paint
		set paint_id [lindex $paint_ids $paint]
		$db eval {INSERT INTO faces(paint) VALUES($paint_id)}
		lappend face_ids [$db last_insert_rowid]
	}

	set vert_ids [list]
	for {set v 0} {$v < $num_verts} {incr v} {
		binary scan [read $ch [* 3 4]] fff x y z
		$db eval {INSERT INTO verts(x, y, z) VALUES($x, $y, $z)}
		lappend vert_ids [$db last_insert_rowid]
	}

	set edge_ids [list]
	set edge_vert_ids [list]
	set next_edges [list]
	set vert_pair_to_edge [dict create]
	for {set f 0} {$f < $num_faces} {incr f} {
		set face_id [lindex $face_ids $f]
		set face_edge_start [llength $edge_ids]
		while {1} {
			binary scan [read $ch 4] iu v
			if {[eof $ch]} {
				error "Not enough data in file!"
			} elseif {$v == 0xffffffff} {
				lappend next_edges $face_edge_start
				break
			} elseif {[llength $edge_ids] != $face_edge_start} {
				lappend next_edges [llength $edge_ids]
			}
			set vert_id [lindex $vert_ids $v]
			$db eval {INSERT INTO edges(vert, face) VALUES($vert_id, $face_id)}
			lappend edge_ids [$db last_insert_rowid]
			lappend edge_vert_ids $vert_id
		}
	}

	# Link twins
	for {set i 0} {$i < [llength $edge_ids]} {incr i} {
		set next_i [lindex $next_edges $i]
		set edge_id [lindex $edge_ids $i]
		set vert_id [lindex $edge_vert_ids $i]
		set next_vert_id [lindex $edge_vert_ids $next_i]
		set vert_pair [list $vert_id $next_vert_id]
		if {[dict exists $vert_pair_to_edge $vert_pair]} {
			set twin_id [dict get $vert_pair_to_edge $vert_pair]
			$db eval {UPDATE edges SET twin = $edge_id WHERE rowid = $twin_id}
			$db eval {UPDATE edges SET twin = $twin_id WHERE rowid = $edge_id}
		} else {
			dict set vert_pair_to_edge [list $next_vert_id $vert_id] $edge_id
		}
	}

	convert_sel_list $ch $face_ids $db {INSERT INTO sel_faces VALUES($id)}
	convert_sel_list $ch $vert_ids $db {INSERT INTO sel_verts VALUES($id)}
	convert_sel_list $ch $edge_ids $db {INSERT INTO sel_edges VALUES($id)}

	binary scan [read $ch 4] iu sel_mode
	binary scan [read $ch 4] iu grid_on
	binary scan [read $ch 4] f grid_size
	binary scan [read $ch [* 3 4]] fff work_org_x work_org_y work_org_z
	binary scan [read $ch [* 3 4]] fff work_norm_x work_norm_y work_norm_z
	seek $ch 4 current ;# reserved
	$db eval {
		INSERT INTO editor(
			sel_mode, grid_on, grid_size,
			work_org_x, work_org_y, work_org_z,
			work_norm_x, work_norm_y, work_norm_z
		) VALUES(
			$sel_mode, $grid_on, $grid_size,
			$work_org_x, $work_org_y, $work_org_z,
			$work_norm_x, $work_norm_y, $work_norm_z
		)
	}

	binary scan [read $ch [* 3 4]] fff cam_x cam_y cam_z
	binary scan [read $ch [* 2 4]] ff rot_x rot_y
	binary scan [read $ch 4] f zoom
	binary scan [read $ch 4] iu view_mode
	binary scan [read $ch 4] iu pick_type
	$db eval {
		INSERT INTO views(cam_x, cam_y, cam_z, rot_x, rot_y, zoom, view_mode, pick_type)
		VALUES($cam_x, $cam_y, $cam_z, $rot_x, $rot_y, $zoom, $view_mode, $pick_type)
	}

	set lib_uuid_to_id [dict create]
	while {1} {
		set lib_path [read_string $ch]
		if {$lib_path == ""} {
			break
		}
		set lib_uuid [read $ch 16]
		$db eval {INSERT INTO library(path) VALUES($lib_path)}
		dict set lib_uuid_to_id $lib_uuid [$db last_insert_rowid]
	}
	for {set i 0} {$i < [llength $paint_ids]} {incr i} {
		set paint_id [lindex $paint_ids $i]
		set mat_uuid [lindex $paint_mat_uuids $i]
		if {[dict exists $lib_uuid_to_id $mat_uuid]} {
			set lib_id [dict get $lib_uuid_to_id $mat_uuid]
			$db eval {UPDATE paints SET material = $lib_id WHERE rowid = $paint_id}
		}
	}

	if {[eof $ch]} {
		error "Not enough data in file!"
	}
	read $ch 1
	if {! [eof $ch]} {
		puts "WARNING: Extra data at end of file!"
	}

	$db eval {COMMIT TRANSACTION}
}

if {$argc < 2} {
	puts "Usage: $argv0 input.wing output.db"
	exit 1
}
lassign $argv in_path out_path

puts "Reading: $in_path"
set in_ch [open $in_path r]
fconfigure $in_ch -translation binary

puts "Writing: $out_path"
file delete $out_path
sqlite3 out_db $out_path

convert_winged_file $in_ch out_db

close $in_ch
out_db close
puts "Success"
