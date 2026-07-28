
$fn=60;

module xiao(){
translate([-74,-17.7,2.16]) import("/home/mauricio/Documents/PlatformIO/Projects/servo-motion-controller/3dmodels/Seeed Studio XIAO ESP32C3.stl");
}


%translate([5,0]) xiao();
difference(){
    linear_extrude(10) offset(1) offset(-1) square([40,20],center=true);
   translate([5,0,2]) linear_extrude(10) offset(.2) projection(cut=true) translate([0,0,-3]) hull() xiao();
    
    
    translate([36,15]/2) cylinder(d=2,h=30,center=true);
    translate([-36,15]/2) cylinder(d=2,h=30,center=true);
    translate([36,-15]/2) cylinder(d=2,h=30,center=true);
    translate([-36,-15]/2) cylinder(d=2,h=30,center=true);
    
    translate([0,0,10-1.5]) linear_extrude(5) offset(1) offset(-1) square([40-1,20-1],center=true);
    
    translate([11,0,5]) rotate([0,90])linear_extrude(10) hull(){
        translate([0,3]) circle(d=7);
        translate([0,-3]) circle(d=7);
    }
    
   translate([0,0,4]) rotate([0,-90]) hull(){
        cylinder(d=3,h=30);
        translate([10,0]) cylinder(d=3,h=30);
    }
    
    translate([-4.3,0,2]) cylinder(d=5,h=10);
    
translate([-9,0,6]) cylinder(d=18,h=10);
    
    
    translate([36,15]/2) cylinder(d=8,h=2,center=true);
    translate([-36,15]/2) cylinder(d=8,h=2,center=true);
    translate([36,-15]/2) cylinder(d=8,h=2,center=true);
    translate([-36,-15]/2) cylinder(d=8,h=2,center=true);
    
    
}