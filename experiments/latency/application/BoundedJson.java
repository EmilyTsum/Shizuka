// SPDX-License-Identifier: GPL-2.0-only
import java.math.BigDecimal;
import java.util.*;

/** Strict bounded reader for fixed trusted helper output; duplicate keys never overwrite. */
final class BoundedJson {
    private final String text;private int at,items;
    private BoundedJson(String text){if(text==null||text.length()>65536)throw new IllegalArgumentException("JSON exceeds bound");this.text=text;}
    static Map<String,Object> object(String text){BoundedJson p=new BoundedJson(text);Object value=p.value(0);p.space();if(p.at!=text.length()||!(value instanceof Map))throw new IllegalArgumentException("Expected one JSON object");@SuppressWarnings("unchecked")Map<String,Object> out=(Map<String,Object>)value;return out;}
    private void space(){while(at<text.length()&&" \n\r\t".indexOf(text.charAt(at))>=0)at++;}
    private char take(){if(at>=text.length())throw bad();return text.charAt(at++);}
    private IllegalArgumentException bad(){return new IllegalArgumentException("Malformed bounded JSON");}
    private Object value(int depth){
        space();if(depth>16||++items>4096||at>=text.length())throw bad();char c=text.charAt(at);
        if(c=='"')return string();
        if(c=='{'){at++;Map<String,Object> out=new LinkedHashMap<>();space();if(at<text.length()&&text.charAt(at)=='}'){at++;return out;}while(true){space();if(at>=text.length()||text.charAt(at)!='"')throw bad();String key=string();space();if(take()!=':'||out.containsKey(key))throw bad();out.put(key,value(depth+1));space();char end=take();if(end=='}')return out;if(end!=',')throw bad();}}
        if(c=='['){at++;List<Object> out=new ArrayList<>();space();if(at<text.length()&&text.charAt(at)==']'){at++;return out;}while(true){out.add(value(depth+1));space();char end=take();if(end==']')return out;if(end!=',')throw bad();}}
        for(String literal:new String[]{"true","false","null"})if(text.startsWith(literal,at)){at+=literal.length();return literal.equals("null")?null:literal.equals("true");}
        int start=at;while(at<text.length()&&"-+0123456789.eE".indexOf(text.charAt(at))>=0)at++;
        String number=text.substring(start,at);if(!number.matches("-?(0|[1-9][0-9]*)(\\.[0-9]+)?([eE][+-]?[0-9]+)?")||number.length()>64)throw bad();
        try{return number.indexOf('.')<0&&number.indexOf('e')<0&&number.indexOf('E')<0?Long.valueOf(number):new BigDecimal(number);}catch(NumberFormatException invalid){throw bad();}
    }
    private String string(){if(take()!='"')throw bad();StringBuilder out=new StringBuilder();while(true){char c=take();if(c=='"')return out.toString();if(c<32)throw bad();if(c=='\\'){c=take();switch(c){case '"':case '\\':case '/':out.append(c);break;case 'b':out.append('\b');break;case 'f':out.append('\f');break;case 'n':out.append('\n');break;case 'r':out.append('\r');break;case 't':out.append('\t');break;case 'u':if(at+4>text.length())throw bad();String hex=text.substring(at,at+4);if(!hex.matches("[a-fA-F0-9]{4}"))throw bad();out.append((char)Integer.parseInt(hex,16));at+=4;break;default:throw bad();}}else out.append(c);if(out.length()>16384)throw bad();}}
}
