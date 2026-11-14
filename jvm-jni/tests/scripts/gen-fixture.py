from pathlib import Path

root = Path(__file__).resolve().parents[3]
d = root / "jvm-jni/build/native/generated/java/conformance"
d.mkdir(parents=True, exist_ok=True)
types = [
    ("Boolean", "boolean", "jboolean", "Z", "z", "JNI_TRUE"),
    ("Byte", "byte", "jbyte", "B", "b", "-7"),
    ("Char", "char", "jchar", "C", "c", "0x4e2d"),
    ("Short", "short", "jshort", "S", "s", "-2000"),
    ("Int", "int", "jint", "I", "i", "12345"),
    ("Long", "long", "jlong", "J", "j", "INT64_C(0x123456789abc)"),
    ("Float", "float", "jfloat", "F", "f", "1.25f"),
    ("Double", "double", "jdouble", "D", "d", "2.5"),
    ("Object", "Object", "jobject", "Ljava/lang/Object;", "l", "str"),
]
params = ", ".join(j + " " + var for name, j, c, s, var, v in types)
j = [
    "package conformance;",
    "public class Box {",
    "public static int calls;",
    "public Box() {}",
    f"public Box({params}) {{"
    + "".join(f"this.{v}={v};" for _, _, _, _, v, _ in types)
    + "}",
]
for name, jt, ct, s, var, val in types:
    j += [
        f"public {jt} {var}; public static {jt} s{var};",
        f"public {jt} m{name}({params}) {{return {var};}}",
        f"public static {jt} sm{name}({params}) {{return {var};}}",
    ]
j += [
    f"public void mVoid({params}) {{calls++;}}",
    f"public static void smVoid({params}) {{calls++;}}",
    "}",
]
(d / "Box.java").write_text("\n".join(j) + "\n")
(d / "Child.java").write_text(
    "package conformance; public class Child extends Box {public Child(){} @Override public int mInt("
    + params
    + "){return i+1000;}}\n"
)
(d / "Dynamic.java").write_text(
    "package conformance; public class Dynamic { public int value(){return 71;} }\n"
)
# Native method matrix: every primitive/object/void return, three dispatch kinds and three argument forms.
c = []
alltypes = types + [("Void", "void", "void", "V", "", "")]
sigargs = "ZBCSIJFDLjava/lang/Object;"
for name, jt, ct, s, var, val in alltypes:
    for family in ["", "Static", "Nonvirtual"]:
        decl = (
            "JNIEnv*e,jobject obj,"
            + ("jclass cls," if family == "Nonvirtual" else "")
            + "jmethodID m,..."
        )
        args = "e,obj," + ("cls," if family == "Nonvirtual" else "") + "m,va"
        c.append(
            f"static {ct} cv_{family}{name}({decl}) {{va_list va;va_start(va,m);"
            + (f"{ct} r=" if ct != "void" else "")
            + f"(*e)->Call{family}{name}MethodV({args});va_end(va);"
            + ("return r;" if ct != "void" else "")
            + "}"
        )
c.append("static int method_matrix(JNIEnv*e) {")
c.append(
    'jclass box=(*e)->FindClass(e,"conformance/Box");C(box);jclass child=(*e)->FindClass(e,"conformance/Child");C(child);'
)
c.append(
    'jmethodID ctor=(*e)->GetMethodID(e,child,"<init>","()V");C(ctor);jobject obj=(*e)->NewObject(e,child,ctor);E();C(obj);jstring str=(*e)->NewStringUTF(e,"arg");C(str);'
)
c.append(
    "jvalue a[9]={{.z=JNI_TRUE},{.b=-7},{.c=0x4e2d},{.s=-2000},{.i=12345},{.j=INT64_C(0x123456789abc)},{.f=1.25f},{.d=2.5},{.l=str}};"
)
values = ",".join(val for _, _, _, _, _, val in types)
for name, jt, ct, s, var, val in alltypes:
    for family in ["", "Static", "Nonvirtual"]:
        methodname = ("sm" if family == "Static" else "m") + name
        lookup = "GetStaticMethodID" if family == "Static" else "GetMethodID"
        c.append(
            "{jmethodID m=(*e)->"
            + lookup
            + f'(e,box,"{methodname}","({sigargs}){s}");C(m);'
        )
        objarg = "box" if family == "Static" else "obj"
        for variant in ["", "A", "V"]:
            if variant == "V":
                call = (
                    f"cv_{family}{name}(e,{objarg},"
                    + ("box," if family == "Nonvirtual" else "")
                    + f"m,{values})"
                )
            else:
                call = (
                    f"(*e)->Call{family}{name}Method{variant}(e,{objarg},"
                    + ("box," if family == "Nonvirtual" else "")
                    + "m,"
                    + ("a" if variant == "A" else values)
                    + ")"
                )
            if ct == "void":
                c.append(call + ";E();")
            elif name == "Object":
                c.append(
                    "{jobject v="
                    + call
                    + ";E();C((*e)->IsSameObject(e,v,str));(*e)->DeleteLocalRef(e,v);}"
                )
            else:
                expected = "13345" if name == "Int" and family == "" else val
                c.append(
                    "{"
                    + ct
                    + " v="
                    + call
                    + f";E();C(v==({ct})({expected}));"
                    + "}"
                )
        c.append("}")
c.append(
    'jfieldID calls=(*e)->GetStaticFieldID(e,box,"calls","I");C(calls);C((*e)->GetStaticIntField(e,box,calls)==9);return 0;}'
)
c.append(
    'static int fields_arrays(JNIEnv*e) {jclass box=(*e)->FindClass(e,"conformance/Box");C(box);jobject obj=(*e)->AllocObject(e,box);C(obj);jstring str=(*e)->NewStringUTF(e,"field");C(str);'
)
for name, jt, ct, s, var, val in types:
    for static in [False, True]:
        prefix = "Static" if static else ""
        recv = "box" if static else "obj"
        fieldname = ("s" if static else "") + var
        c.append(
            "{jfieldID id=(*e)->Get"
            + prefix
            + f'FieldID(e,box,"{fieldname}","{s}");C(id);(*e)->Set{prefix}{name}Field(e,{recv},id,({ct})({val}));E();'
        )
        if name == "Object":
            c.append(
                f"jobject v=(*e)->Get{prefix}{name}Field(e,{recv},id);C((*e)->IsSameObject(e,v,str));(*e)->DeleteLocalRef(e,v);"
            )
        else:
            c.append(
                f"C((*e)->Get{prefix}{name}Field(e,{recv},id)==({ct})({val}));"
            )
        c.append("}")
for name, jt, ct, s, var, val in types[:-1]:
    c.append(
        "{"
        + f"{ct} values[3]={{({ct})1,({ct})2,({ct})3}},out[3]={{0}};j{name.lower()}Array arr=(*e)->New{name}Array(e,3);C(arr);C((*e)->GetArrayLength(e,arr)==3);"
    )
    # Boolean needs values 0/1; all other types round trip.
    if name == "Boolean":
        c.append("values[0]=JNI_TRUE;values[1]=JNI_FALSE;values[2]=JNI_TRUE;")
    c.append(
        f"(*e)->Set{name}ArrayRegion(e,arr,0,3,values);E();(*e)->Get{name}ArrayRegion(e,arr,0,3,out);E();C(!memcmp(values,out,sizeof values));"
    )
    c.append(
        f"jboolean copy=JNI_FALSE;{ct}*p=(*e)->Get{name}ArrayElements(e,arr,&copy);C(p);p[0]=({ct})0;(*e)->Release{name}ArrayElements(e,arr,p,JNI_COMMIT);if(!copy)p=(*e)->Get{name}ArrayElements(e,arr,&copy);C(p);p[1]=({ct})1;(*e)->Release{name}ArrayElements(e,arr,p,0);"
    )
    c.append(
        f"(*e)->Get{name}ArrayRegion(e,arr,0,3,out);C(out[0]==0&&out[1]==1);p=(*e)->Get{name}ArrayElements(e,arr,NULL);C(p);(*e)->Release{name}ArrayElements(e,arr,p,JNI_ABORT);(*e)->DeleteLocalRef(e,arr);"
        + "}"
    )
c.append("return 0;}")
(root / "jvm-jni/build/native/generated").mkdir(parents=True, exist_ok=True)
(root / "jvm-jni/build/native/generated/fixture_generated.inc").write_text(
    "\n".join(c) + "\n"
)
print("Fixture method/field/array matrix generated")
