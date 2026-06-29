// Test: switch statement
public class SwitchTest {

    public enum Day {
        MONDAY, TUESDAY, WEDNESDAY, THURSDAY, FRIDAY, SATURDAY, SUNDAY
    }

    public static void main(String[] args) {
        Day today = Day.MONDAY;
        switch (today) {
            case MONDAY:
                System.out.println("Start of work week");
                break;
            case FRIDAY:
                System.out.println("TGIF");
                break;
            default:
                System.out.println("Midweek");
                break;
        }

        System.out.println("=== Switch: PASS ===");
    }
}
